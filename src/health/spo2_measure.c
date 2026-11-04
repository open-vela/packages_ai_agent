/****************************************************************************
 * spo2_measure.c - One-shot blood-oxygen measurement behind the pet page button.
 *
 * 设计要点见 spo2_measure.h。这里只补充两个实现层面的坑：
 *
 * 1) **不能在 LVGL render 线程里跑采集**。spo2 一轮是 ~19 秒的阻塞采集，
 *    而本工程的 LVGL 是 LV_OS_NONE（渲染线程就是唯一 UI 线程），
 *    在那里阻塞会让整个界面卡死。所以点击回调只调 spo2_measure_start()，
 *    它创建一个 worker 线程就返回。
 *
 * 2) **结果通过全局变量取，不用返回值**。hr_sensor_main 是 main() 形态，
 *    只返回 int；具体数值由 max30102.c 的 spo2_measure() 填进
 *    g_max30102_spo2_{r,pct,ok}。两边在同一个 --start-group 里，符号可解析。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "health/spo2_measure.h"

#include "core/message_bus.h"   /* AGENT_MSG_KIND_* */
#include "ui/lvgl_ui_channel.h" /* lvgl_ui_post / lvgl_ui_channel_send_kind */
#include "ui/pet_page.h"        /* pet_page_update_response */

/* hr_sensor 内建程序的入口（由 -Dmain=hr_sensor_main 改名而来） */
extern int hr_sensor_main(int argc, FAR char *argv[]);

/* max30102.c 暴露的最近一次血氧结果 */
extern float g_max30102_spo2_r;
extern float g_max30102_spo2_pct;
extern int   g_max30102_spo2_ok;

/****************************************************************************
 * Private Data
 ****************************************************************************/

#define SPO2_TAG        "spo2_measure"
#define SPO2_STACK      16384
#define SPO2_PRIORITY   100

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_busy;

/* 进度文本（worker 写，render 线程读） */
static volatile int s_progress_left_s;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void progress_async_cb(void *data)
{
    char buf[32];

    (void)data;
    /* 测量中 Ns */
    snprintf(buf, sizeof(buf),
             "\xe6\xb5\x8b\xe9\x87\x8f\xe4\xb8\xad %ds",
             s_progress_left_s);
    pet_page_update_response(buf);
}

static void greeting_restore_async_cb(void *data)
{
    (void)data;
    /* 结果留在屏幕上，不立刻抢回问候语 —— 血氧数比问候语重要。
     * 下一次主动关怀/交互会自然覆盖它。 */
}

/**
 * 调 hr_sensor 的 spo2 子命令。
 *
 * 直接在本线程同步调用（不走 task_spawn）：spo2 是纯顺序流程，没有常驻
 * 采集线程，不需要把 stop 请求送进另一个任务。这样也不用处理子进程退出码。
 */
static int spo2_run_hw(void)
{
    char *argv[3];

    argv[0] = (char *)"hr_sensor";
    argv[1] = (char *)"spo2";
    argv[2] = NULL;

    return hr_sensor_main(2, argv);
}

static void *spo2_worker(void *arg)
{
    char text[96];
    int hw_ret;
    int i;

    (void)arg;

    /* 每 5 秒刷一次进度（19 秒的采集，用户需要知道还在跑） */
    for (i = 0; i < 4; i++) {
        s_progress_left_s = 20 - i * 5;
        lvgl_ui_post(progress_async_cb, NULL);
        usleep(1000 * 1000);
    }

    hw_ret = spo2_run_hw();

    if (hw_ret == 0 && g_max30102_spo2_ok) {
        /* 您的血氧是 NN% */
        snprintf(text, sizeof(text),
                 "\xe6\x82\xa8\xe7\x9a\x84\xe8\xa1\x80\xe6\xb0\xa7"
                 "\xe6\x98\xaf %d%%",
                 (int)(g_max30102_spo2_pct + 0.5f));
        printf("[%s] ok: spo2=%.0f R=%.3f\n", SPO2_TAG,
               g_max30102_spo2_pct, g_max30102_spo2_r);
        lvgl_ui_channel_send_kind(text, AGENT_MSG_KIND_FINAL);
    } else {
        /* 诚实提示，不编数：调制比不在生理范围 / R 超出 / 采集失败都走这里 */
        printf("[%s] failed: hw_ret=%d ok=%d\n", SPO2_TAG, hw_ret,
               g_max30102_spo2_ok);
        lvgl_ui_channel_send_kind(
            "\xe8\xa1\x80\xe6\xb0\xa7\xe6\xb5\x8b\xe9\x87\x8f\xe6\x9c\xaa\xe6\x88\x90\xe5\x8a\x9f"
            "\xef\xbc\x8c\xe8\xaf\xb7\xe6\x8a\x8a\xe6\x89\x8b\xe6\x8c\x87\xe8\xb4\xb4\xe7\xa8\xb3\xe4\xbc\xa0\xe6\x84\x9f\xe5\x99\xa8",
            AGENT_MSG_KIND_ERROR);
    }

    lvgl_ui_post(greeting_restore_async_cb, NULL);

    pthread_mutex_lock(&s_lock);
    s_busy = false;
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int spo2_measure_start(void)
{
    pthread_attr_t attr;
    pthread_t tid;
    int ret;

    pthread_mutex_lock(&s_lock);
    if (s_busy) {
        pthread_mutex_unlock(&s_lock);
        return -EBUSY;
    }
    s_busy = true;
    pthread_mutex_unlock(&s_lock);

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, SPO2_STACK);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    ret = pthread_create(&tid, &attr, spo2_worker, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        printf("[%s] pthread_create failed: %d\n", SPO2_TAG, ret);
        pthread_mutex_lock(&s_lock);
        s_busy = false;
        pthread_mutex_unlock(&s_lock);
        return -ret;
    }

    printf("[%s] measurement started\n", SPO2_TAG);
    return 0;
}

bool spo2_measure_busy(void)
{
    bool busy;

    pthread_mutex_lock(&s_lock);
    busy = s_busy;
    pthread_mutex_unlock(&s_lock);
    return busy;
}
