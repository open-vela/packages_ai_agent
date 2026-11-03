/****************************************************************************
 * hr_watch.c - 后台静默心率守护（真·心率告警的数据源）
 *
 * 设计（对应任务书的四点要求）：
 *
 *  1) hr_watch_start() 幂等起一个低频守护线程（默认 10 分钟一轮，
 *     HR_WATCH_INTERVAL_S 可调）。线程常驻、几乎全程在 sleep，
 *     真正的测量全部复用 hr_measure 自己的 worker —— 本模块不碰
 *     传感器、不碰 uORB、不碰 I2C。
 *
 *  2) 每轮：没有测量在进行（含用户手动测量）就调
 *     hr_measure_start_silent() 起一次**静默**测量 —— 与按钮路径
 *     同一套采集/质量判据/收尾，但不上"测量中"进度、结果不上屏、
 *     不进 TTS（后台测量不能打扰用户）。然后 1s 间隔轮询
 *     is_running() 由 true 变 false，读结果分诊：
 *       失败( last_ok==false 或 bpm<=0 ) -> 静默跳过，累计失败次数，
 *                                          连续 >3 次打 syslog 警告
 *       bpm >= HR_WATCH_BPM_HIGH(120)    -> pet_care_hr_report(bpm)
 *       其余                              -> 仅 syslog 记录
 *
 *  3) 节流不在这里造：pet_care_hr_report() 内部已有 60s 告警冷却和
 *     免打扰感知（pet_care.c 的 s_last_hr_alert_s / care_is_quiet），
 *     本模块只把真实数值递过去。
 *
 *  4) 诚实性红线：没测到（失败 / bpm==0）绝不告警、绝不显示 0 ——
 *     只跳过。判断"测到没有"以 hr_measure_last_ok() 为准：失败轮次里
 *     hr_measure_last_bpm() 可能残留最后一次 copy 到的非零值，单看
 *     bpm 会把失败轮误当成功轮（详见 hr_measure.c 的 s_last_ok 注释）。
 *
 * 线程安全：pet_care_hr_report() 内部自带互斥，其下游 care_fire /
 * lvgl_ui_channel_log / pet_display_set_emotion 全部经 malloc +
 * lvgl_ui_post 互斥队列投递回 render 线程，从本线程调用是安全的
 * （Round 27 之后本仓所有跨线程 LVGL 访问都必须走这条路）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <syslog.h>
#include <unistd.h>

#include "health/hr_measure.h" /* 静默起测 + 结果读取 */
#include "health/hr_watch.h"
#include "ui/pet_care.h"       /* pet_care_hr_report: 告警出口 */

#define TAG "hr_watch"

/* 守护线程栈：本线程只做「起测 / 轮询 / 分诊」，真正重的测量跑在
 * hr_measure 自己的 worker 线程（那边 16KB）。8KB 给 syslog 的
 * printf 格式化留足余量。 */
#define HR_WATCH_STACK 8192

/* 开机后第一次检查前的延时：让启动期（网络/TTS/表盘首帧）先跑完，
 * 也给用户戴上手表的时间。 */
#define HR_WATCH_FIRST_DELAY_S 60

/* 等一轮测量结束的兜底上限。hr_measure 内部硬上限 30s
 * (HR_MEASURE_MAX_S)，加上起停开销（start 最多 8s + stop 最多 3s
 * + settle 0.2s），60s 覆盖所有正常路径；到点还在跑说明 worker
 * 异常挂住了，本轮作废。 */
#define HR_WATCH_MEASURE_TIMEOUT_S 60

/* 连续失败超过多少次后开始打 syslog 警告（>3 即第 4 次起，
 * 按任务书"累计失败计数，>3 次打 syslog 警告"）。 */
#define HR_WATCH_FAIL_WARN_N 3

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_started;

/****************************************************************************
 * Private
 ****************************************************************************/

static void *hr_watch_thread(void *arg)
{
    unsigned sleep_s = HR_WATCH_FIRST_DELAY_S;
    int fails = 0;   /* 连续失败计数：成功一次即清零 */

    (void)arg;

    syslog(LOG_INFO, "[%s] watcher started (interval=%ds high=%d first=%ds)\n",
           TAG, HR_WATCH_INTERVAL_S, HR_WATCH_BPM_HIGH,
           HR_WATCH_FIRST_DELAY_S);

    for (;;) {
        int ret;
        int waited;
        int bpm;

        sleep(sleep_s);
        sleep_s = HR_WATCH_INTERVAL_S;   /* 首轮延时之后恢复常规周期 */

        /* 用户自己正在测（按钮路径）→ 这轮让给用户：不抢传感器，
         * 也不把用户那轮的结果当后台数据分诊。 */
        if (hr_measure_is_running()) {
            continue;
        }

        ret = hr_measure_start_silent();
        if (ret == -EBUSY) {
            /* 检查完 is_running 到真正起测之间用户刚好点了按钮：
             * 同上，让给用户，不算失败。 */
            continue;
        }
        if (ret != 0) {
            fails++;
            if (fails > HR_WATCH_FAIL_WARN_N) {
                syslog(LOG_WARNING,
                       "[%s] start_silent failed %d times in a row (ret=%d)\n",
                       TAG, fails, ret);
            }
            continue;
        }

        /* 等测量结束：is_running() 由 true 轮询到 false，间隔 1s */
        for (waited = 0;
             hr_measure_is_running() && waited < HR_WATCH_MEASURE_TIMEOUT_S;
             waited++) {
            sleep(1);
        }

        if (hr_measure_is_running()) {
            /* 兜底：worker 挂死（下层 I2C 卡死之类）。本轮作废，
             * 不读结果、不计失败（属于另一类异常，下一轮自然重试）。 */
            syslog(LOG_WARNING,
                   "[%s] measurement still running after %ds, skip round\n",
                   TAG, HR_WATCH_MEASURE_TIMEOUT_S);
            continue;
        }

        /* ★ 先看成败标志再看数值：失败轮里 last_bpm() 可能残留
         * 最后 copy 到的非零值，绝不能拿它分诊（诚实性红线）。 */
        if (!hr_measure_last_ok()) {
            fails++;
            if (fails > HR_WATCH_FAIL_WARN_N) {
                syslog(LOG_WARNING,
                       "[%s] no reading %d times in a row (watch off wrist?)\n",
                       TAG, fails);
            }
            continue;
        }

        bpm = hr_measure_last_bpm();
        if (bpm <= 0) {
            /* 双保险：成功轮理论上必有非零值；万一读到 0（比如刚好的
             * 竞态窗口里有新一轮测量把缓存清了），宁可不报也不猜。 */
            fails++;
            if (fails > HR_WATCH_FAIL_WARN_N) {
                syslog(LOG_WARNING,
                       "[%s] ok but bpm=%d, treated as no reading (fails=%d)\n",
                       TAG, bpm, fails);
            }
            continue;
        }

        fails = 0;   /* 测到了：连续失败计数清零 */

        if (bpm >= HR_WATCH_BPM_HIGH) {
            /* 告警出口内部自带 60s 冷却 + 免打扰感知，这里不重复节流；
             * 免打扰期间 pet_care_hr_report 也已处理，无需额外判断。 */
            syslog(LOG_WARNING, "[%s] high bpm=%d, reporting\n", TAG, bpm);
            pet_care_hr_report(bpm);
        } else {
            syslog(LOG_INFO, "[%s] bpm=%d normal\n", TAG, bpm);
        }
    }

    /* 不可达：守护线程无停止接口，随系统生命周期存在 */
    return NULL;
}

/****************************************************************************
 * Public API
 ****************************************************************************/

int hr_watch_start(void)
{
    pthread_attr_t attr;
    pthread_t tid;
    int ret;

    pthread_mutex_lock(&s_lock);
    if (s_started) {
        pthread_mutex_unlock(&s_lock);
        return 0;   /* 幂等：重复调用直接返回成功 */
    }
    s_started = true;
    pthread_mutex_unlock(&s_lock);

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HR_WATCH_STACK);
    /* detach：守护线程自生自灭，调用方（render 线程）绝不等待 */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    ret = pthread_create(&tid, &attr, hr_watch_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        syslog(LOG_ERR, "[%s] pthread_create failed: %d\n", TAG, ret);
        pthread_mutex_lock(&s_lock);
        s_started = false;   /* 允许下次重试 */
        pthread_mutex_unlock(&s_lock);
        return -ENOMEM;
    }

    return 0;
}

bool hr_watch_is_running(void)
{
    bool started;

    pthread_mutex_lock(&s_lock);
    started = s_started;
    pthread_mutex_unlock(&s_lock);

    return started;
}
