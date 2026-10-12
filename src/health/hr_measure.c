/****************************************************************************
 * hr_measure.c - One-shot heart-rate measurement (P1b).
 *
 * 桌宠页「测心率」按钮背后的单次测量服务。设计要点：
 *
 *  1) **不占用 LVGL render 线程**。LVGL 在本工程以 LV_OS_NONE 构建（无内部锁，
 *     见 lvgl_ui_channel.c:199-208），render 线程每 5ms 跑一次 lv_timer_handler。
 *     点击回调只做「起一个 worker 线程」这一件事，立刻返回；8~15s 的测量、
 *     poll、等 task_spawn 全部发生在 worker 线程里。所有 LVGL 写操作都经
 *     lvgl_ui_post() 投递回 render 线程。
 *
 *  2) **起停采集复用已有 CLI 路径**（P1a 已修好并真机验证）：
 *     task_spawn("hr_start"/"hr_stop", hr_sensor_main, ..., {"start"/"stop"}, ...)。
 *     hr_sensor_main 是真实符号（-Dmain=hr_sensor_main，见 max30102.c 的编译命令），
 *     libapps_hr_sensor.a 与 libapps_ai_agent.a 同在链接 --start-group 内。
 *     走这条路而不是重写一份 I2C 初始化：P1a 修好的 SHDN/close 退出路径在
 *     采集线程自己身上，任何旁路实现都会漏掉「关 LED / 释放总线」。
 *
 *  3) **订阅自己 poll 并缓存**。orb_read_once()（tool_health.c:41-74）是
 *     「先等新更新再 copy」，stop 之后不再有更新就会超时失败；这里持有 fd
 *     持续 poll，并把**最后一次成功 copy 的值**留在 s_last_bpm 里。
 *
 *  4) 结束时**一定**发 stop（成功、失败、提前放弃三条路径都走同一个出口），
 *     保证 LED 灭、总线释放。
 *
 * 质量判据：algo_heartrate 只有 timestamp_us / bpm / heartrate_section[3]
 * 五个字段，**没有 q/gate**（质量门控全在发布端 max30102.c 内部，不外露），
 * 所以用「非零 bpm + 同量级的值在 3s 内再次到达」这一可用判据，
 * 详见 hr_measure_worker 的注释。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "health/hr_measure.h"

#if defined(CONFIG_UORB) && defined(CONFIG_FITNESS_ALGO)
#  if __has_include(<fitness/algo_heartrate.h>)
#    define HR_MEASURE_HAVE_TOPIC 1
#  endif
#endif

#ifdef HR_MEASURE_HAVE_TOPIC

#include <spawn.h>
#include <uORB/uORB.h>
#include <fitness/algo_heartrate.h>

#include "core/message_bus.h"   /* AGENT_MSG_KIND_* */
#include "ui/lvgl_ui_channel.h" /* lvgl_ui_post / lvgl_ui_channel_send_kind */
#include "ui/pet_page.h"        /* pet_page_update_response / greeting */

/* hr_sensor 内建程序的入口（apps/packages/demos/.../sensors/max30102.c 的 main，
 * 由 -Dmain=hr_sensor_main 改名而来；nm nuttx 里是 T hr_sensor_main）。 */
extern int hr_sensor_main(int argc, FAR char *argv[]);

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* 单飞：worker 存活期间为 true。用互斥量而不是裸 bool —— 点击回调在 LVGL
 * 线程，worker 自己在另一个线程，两边都要读写。 */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_busy;

/* worker 栈：显式 16KB（任务要求 >=16KB）。这条线程要跑 task_spawn +
 * 等退出 + poll，还要在内部用 snprintf 拼文案，不能吃默认栈。 */
#define HR_MEASURE_STACK   16384
/* hr_sensor 内建程序的栈（与 CMakeLists 里 STACKSIZE 8192 一致，真机验证过） */
#define HR_SENSOR_STACK    8192

/* 等 hr_sensor 子命令退出的上限。start 要跑 I2C 初始化，给宽一点；
 * stop 只置一个标志，给 3s 足够。 */
#define HR_START_WAIT_MS   8000
#define HR_STOP_WAIT_MS    3000

/* stop 之后留给采集线程收尾的时间：它要在自己的任务组里写 SHDN(关 LED)并
 * close(fd)。这段等待同时保证下一轮 start 时 g_hr.inited 已被清零 ——
 * max30102.c 的 memset 守卫是「!inited && !running」才清零，若上一轮采集
 * 线程还没退出就重新 start，上一轮的 have_bpm/last_bpm 会被留下来，新一轮
 * 会在 ~1s 时把旧值重播一遍。 */
#define HR_STOP_SETTLE_MS  200

/* 结果被接受前的最短测量窗口。max30102.c 的 have_bpm 由 start 进程的 memset
 * 清零，正常一轮测量是干净的；这里的下限一是挡住上面那条重启竞态的残余值，
 * 二是给算法留出重新锁定真值的时间（warmup 0.8s + 3 个 RR + 连续确认）。 */
#define HR_MEASURE_MIN_MS  4000
/* 合理心率区间（发布端已按 40..200 校验过，这里只是兜底） */
#define HR_BPM_SANE_MIN    30
#define HR_BPM_SANE_MAX    220

typedef enum {
    HR_ST_IDLE = 0,
    HR_ST_STARTING,
    HR_ST_MEASURING,
    HR_ST_DONE,
    HR_ST_FAILED
} hr_state_t;

static volatile hr_state_t s_state = HR_ST_IDLE;
static volatile uint32_t  s_last_bpm;   /* 最后一次成功 copy 到的 bpm */

/* 进度文本：worker 写，render 线程在 progress_async_cb 里读。单生产者 +
 * 每秒一次 + render 线程 5ms 一圈，不需要额外同步。 */
static volatile int s_progress_left_s;

/****************************************************************************
 * Private helpers
 ****************************************************************************/

static uint32_t hr_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* 两个心率值是否同一量级（+-15%）。全整数运算，值域 30..220 不会溢出。 */
static bool hr_same_scale(uint32_t a, uint32_t b)
{
    uint32_t hi = (a > b) ? a : b;
    uint32_t d  = (a > b) ? (a - b) : (b - a);

    if (hi == 0) {
        return false;
    }
    return ((uint64_t)d * 100u <= (uint64_t)hi * 15u);
}


/* ── 子进程退出码回收 ──────────────────────────────────────────────
 *
 * 为什么不能自己 waitpid(WNOHANG) 轮询：本板 CONFIG_SCHED_WAITPID=y 且
 * **CONFIG_SCHED_HAVE_PARENT 未开**，退出码只在「已经有 waiter 挂在
 * group->tg_statloc 上」时才会被写回（task_exithook.c 的 nxtask_exitwakeup：
 *   if (group->tg_statloc != NULL) *group->tg_statloc = status << 8;
 * ）。轮询式的 WNOHANG 每轮都会把 tg_statloc 置回 NULL，于是**永远读到 0** ——
 * 那样 hr_sensor start 失败（未接传感器/总线打不开）会被误判成成功，
 * 白等满 15s 才报错，而且报的是「请把手指放在传感器上」这个错误原因。
 *
 * 所以这里用一个专门的 reaper 线程做**阻塞** waitpid（它才是真正的 waiter），
 * worker 只轮询结果并自带超时 —— 既拿到真实退出码，又不会把自己挂死。
 *
 * 槽位生命周期：worker 取到结果后立刻 hr_reap_release() 归还槽位，所以正常
 * 一轮测量（start + stop）只用 1 个槽位、可反复复用。只有 worker 超时那条
 * 路径不归还 —— 那时 reaper 还挂在 waitpid 上，归还会让下一个 reaper 与它
 * 争同一个槽。超时是异常路径（下层 I2C 卡死），漏一个槽的代价只是之后
 * 「拿不到退出码」（exitcode=-1，按未知处理），不会误报失败。 */

#define HR_REAP_SLOTS      4

typedef struct {
    pid_t         pid;
    bool          busy;
    volatile bool done;
    volatile int  status;   /* waitpid 的原始 status；-1 表示 waitpid 失败 */
} hr_reap_slot_t;

static hr_reap_slot_t s_reap[HR_REAP_SLOTS];
static pthread_mutex_t s_reap_lock = PTHREAD_MUTEX_INITIALIZER;

static void *hr_reaper_thread(void *arg)
{
    hr_reap_slot_t *slot = (hr_reap_slot_t *)arg;
    int status = 0;
    pid_t w = waitpid(slot->pid, &status, 0);

    /* 顺序不能反：先写 status 再置 done。两者都是 volatile，worker 看到
     * done==true 时 status 一定已经写完（本线程之后不再碰这个槽）。 */
    slot->status = (w == slot->pid) ? status : -1;
    slot->done = true;
    return NULL;
}

/* 归还槽位。只能在确认 reaper 已经退出（done==true 且结果已读走）之后调用：
 * 那时它不会再写这个槽，下一个 reaper 才能安全复用。 */
static void hr_reap_release(hr_reap_slot_t *slot)
{
    if (slot == NULL) {
        return;
    }
    pthread_mutex_lock(&s_reap_lock);
    slot->busy = false;
    pthread_mutex_unlock(&s_reap_lock);
}

/* 取一个空闲槽并起 reaper；返回 NULL 表示槽位用尽（退化为拿不到退出码） */
static hr_reap_slot_t *hr_reap_start(pid_t pid)
{
    pthread_attr_t attr;
    pthread_t tid;
    hr_reap_slot_t *slot = NULL;
    int i;
    int ret;

    pthread_mutex_lock(&s_reap_lock);
    for (i = 0; i < HR_REAP_SLOTS; i++) {
        if (!s_reap[i].busy) {
            slot = &s_reap[i];
            slot->busy = true;
            slot->done = false;
            slot->status = -1;
            slot->pid = pid;
            break;
        }
    }
    pthread_mutex_unlock(&s_reap_lock);

    if (slot == NULL) {
        printf("[hr_measure] no reaper slot free, exit code unavailable\n");
        return NULL;
    }

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    ret = pthread_create(&tid, &attr, hr_reaper_thread, slot);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        printf("[hr_measure] reaper pthread_create failed: %d\n", ret);
        pthread_mutex_lock(&s_reap_lock);
        slot->busy = false;
        pthread_mutex_unlock(&s_reap_lock);
        return NULL;
    }

    return slot;
}

/**
 * 跑一次 hr_sensor 子命令并等它退出。
 *
 * @param name     任务名（ps 里显示用，避免和采集线程的 "hr_sensor" 重名）
 * @param cmd      "start" / "stop"
 * @param wait_ms  等待上限
 * @param exitcode 子命令退出码：
 *                   >= 0 拿到真实退出码
 *                   -1   未拿到（reaper 没赶上/槽位用尽），**不代表失败**
 * @return 0        子命令已退出（退出码见 exitcode）
 *         -ETIMEDOUT 超时（子命令可能仍在跑）
 *         其他负值   task_spawn 失败
 */
static int hr_sensor_cmd(const char *name, const char *cmd, int wait_ms,
                         int *exitcode)
{
    posix_spawnattr_t attr;
    FAR char * const argv[3] = { (FAR char *)"hr_sensor",
                                 (FAR char *)cmd,
                                 NULL };
    hr_reap_slot_t *slot;
    pid_t pid;
    int ret;
    int waited = 0;

    if (exitcode != NULL) {
        *exitcode = -1;
    }

    ret = posix_spawnattr_init(&attr);
    if (ret != 0) {
        return -ret;
    }
    (void)posix_spawnattr_setstacksize(&attr, HR_SENSOR_STACK);

    ret = task_spawn(name, hr_sensor_main, NULL, &attr, argv, NULL);
    posix_spawnattr_destroy(&attr);
    if (ret < 0) {
        printf("[hr_measure] task_spawn(%s) failed: %d\n", cmd, ret);
        return ret;
    }
    pid = (pid_t)ret;

    slot = hr_reap_start(pid);
    if (slot == NULL) {
        /* 没有回收槽：不阻塞，当作「命令已下发但退出码未知」 */
        if (exitcode != NULL) {
            *exitcode = -1;
        }
        return 0;
    }

    while (waited < wait_ms) {
        if (slot->done) {
            if (exitcode != NULL) {
                *exitcode = (slot->status >= 0 &&
                             WIFEXITED(slot->status))
                                ? WEXITSTATUS(slot->status) : -1;
            }
            /* reaper 已退出且结果已读走，槽位可以还给下一次调用 */
            hr_reap_release(slot);
            return 0;
        }
        usleep(20000);
        waited += 20;
    }

    /* 超时：reaper 可能还挂在 waitpid 上，这个槽不能归还（见文件头注释） */
    printf("[hr_measure] hr_sensor %s did not exit in %dms\n", cmd, wait_ms);
    return -ETIMEDOUT;
}


/****************************************************************************
 * LVGL 落地回调（全部在 render 线程执行）
 ****************************************************************************/

static void progress_async_cb(void *data)
{
    char buf[32];

    (void)data;
    /* 测量中 Ns */
    snprintf(buf, sizeof(buf), "\xe6\xb5\x8b\xe9\x87\x8f\xe4\xb8\xad %ds",
             s_progress_left_s);
    pet_page_update_response(buf);
}

/* 结果上屏后再挂一个定时器把时段问候语换回来（仿 pet_page.c 的 greet 定时器） */
static void greeting_restore_async_cb(void *data)
{
    (void)data;
    pet_page_schedule_greeting_restore(HR_MEASURE_GREET_MS);
}

static void hr_post_progress(int left_s)
{
    s_progress_left_s = left_s;
    if (lvgl_ui_post(progress_async_cb, NULL) != 0) {
        /* 队列满：进度丢一条不影响结果，不重试 */
    }
}

/****************************************************************************
 * Result reporting
 ****************************************************************************/

/* 失败原因 -> 上屏文案（都不播报：send_kind 非 FINAL 只渲染，不进 TTS） */

/* 测量失败：未接传感器 */
#define HR_MSG_NO_SENSOR \
    "\xe6\xb5\x8b\xe9\x87\x8f\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x9a" \
    "\xe6\x9c\xaa\xe6\x8e\xa5\xe4\xbc\xa0\xe6\x84\x9f\xe5\x99\xa8"

/* 测量失败：请把手指放在传感器上 */
#define HR_MSG_NO_FINGER \
    "\xe6\xb5\x8b\xe9\x87\x8f\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x9a" \
    "\xe8\xaf\xb7\xe6\x8a\x8a\xe6\x89\x8b\xe6\x8c\x87\xe6\x94\xbe" \
    "\xe5\x9c\xa8\xe4\xbc\xa0\xe6\x84\x9f\xe5\x99\xa8\xe4\xb8\x8a"

/* 测量失败：信号太弱，请重试 */
#define HR_MSG_WEAK \
    "\xe6\xb5\x8b\xe9\x87\x8f\xe5\xa4\xb1\xe8\xb4\xa5\xef\xbc\x9a" \
    "\xe4\xbf\xa1\xe5\x8f\xb7\xe5\xa4\xaa\xe5\xbc\xb1\xef\xbc\x8c" \
    "\xe8\xaf\xb7\xe9\x87\x8d\xe8\xaf\x95"

static void hr_report_ok(uint32_t bpm)
{
    char text[64];

    s_state = HR_ST_DONE;

    /* 一次完成：气泡 + 历史 + 桌宠表情 + 语音播报。
     * AGENT_MSG_KIND_FINAL 是唯一会出声的类别（P2a 的显示/播报解耦点）。 */
    /* 您的心率是 NN */
    snprintf(text, sizeof(text),
             "\xe6\x82\xa8\xe7\x9a\x84\xe5\xbf\x83\xe7\x8e\x87\xe6\x98\xaf"
             " %u",
             (unsigned)bpm);
    printf("[hr_measure] success: bpm=%u\n", (unsigned)bpm);
    lvgl_ui_channel_send_kind(text, AGENT_MSG_KIND_FINAL);
    lvgl_ui_post(greeting_restore_async_cb, NULL);
}

static void hr_report_fail(const char *text, const char *why)
{
    s_state = HR_ST_FAILED;

    printf("[hr_measure] failed: %s (last bpm=%u)\n",
           why, (unsigned)s_last_bpm);
    /* 非 FINAL：上屏 + 表情，但不念（避免吵） */
    lvgl_ui_channel_send_kind(text, AGENT_MSG_KIND_ERROR);
    lvgl_ui_post(greeting_restore_async_cb, NULL);
}


/****************************************************************************
 * Measurement worker
 ****************************************************************************/

/**
 * 测量主循环。
 *
 * 质量判据（algo_heartrate 里没有 q/gate，只能用可用字段）：
 *   「测到了」= 从话题 copy 到 bpm != 0，且**同量级(+-15%)的值在 3s 内再次
 *   到达**（证明发布端仍在更新、算法仍锁定着，不是一次抖动），同时已过
 *   HR_MEASURE_MIN_MS 最短窗口。
 *
 * 提前放弃：见过第一次更新之后连续 HR_MEASURE_STALL_MS 没有新更新。
 *   注意「第一次更新之前」不套用这条 —— 发布端只在 have_bpm 为真时才
 *   orb_publish（max30102.c:2529），所以冷启动时话题在算法锁定之前**一次都
 *   不更新**，从 t=0 起算 3s 会把每一次正常测量都判死。这段时间由 15s
 *   硬上限兜底（手指没放上去时就是走这条路）。
 */
static void hr_measure_worker(void)
{
    struct pollfd pfd;
    uint32_t t0;
    uint32_t last_update = 0;
    uint32_t first_ms = 0;
    uint32_t first_bpm = 0;
    int last_left = -1;
    int fd = -1;
    int exitcode = 0;
    int rc;
    bool have_any = false;
    bool started = false;

    s_state = HR_ST_STARTING;

    /* ── 1) 起采集（既有 CLI 路径） ─────────────────────────────── */
    rc = hr_sensor_cmd("hr_start", "start", HR_START_WAIT_MS, &exitcode);
    printf("[hr_measure] hr_sensor start rc=%d exit=%d\n", rc, exitcode);

    /* exitcode == -1 只表示「没拿到退出码」，不当作失败：hr_sensor start 若
     * 已经成功起好了采集，误报「未接传感器」比多等几秒更糟，而且后面还会
     * 因为拿不到数据被 15s 上限兜住。只有**明确非零**的退出码才提前判定
     * 传感器没接上（hr_start 打不开总线/初始化失败时就是这条路）。 */
    if (rc != 0 || exitcode > 0) {
        hr_report_fail(HR_MSG_NO_SENSOR, "start failed");
        goto out;
    }
    started = true;

    /* ── 2) 订阅（在 start 之后：话题由采集线程 advertise） ──────── */
    fd = orb_subscribe(ORB_ID(algo_heartrate));
    if (fd < 0) {
        printf("[hr_measure] orb_subscribe failed: %d\n", fd);
        hr_report_fail(HR_MSG_NO_SENSOR, "subscribe failed");
        goto out;
    }

    s_state = HR_ST_MEASURING;
    t0 = hr_now_ms();
    pfd.fd = fd;
    pfd.events = POLLIN;

    for (;;) {
        uint32_t now = hr_now_ms();
        uint32_t elapsed = now - t0;
        int left;

        /* 每秒刷一次进度（走 lvgl_ui_post，不写历史环） */
        left = (int)(HR_MEASURE_MAX_S - elapsed / 1000u);
        if (left < 0) {
            left = 0;
        }
        if (left != last_left) {
            last_left = left;
            hr_post_progress(left);
        }

        /* 硬上限 15s */
        if (elapsed >= (uint32_t)HR_MEASURE_MAX_S * 1000u) {
            hr_report_fail(have_any ? HR_MSG_WEAK : HR_MSG_NO_FINGER,
                           have_any ? "timeout, no valid bpm"
                                    : "timeout, no data");
            goto out;
        }

        /* 提前放弃：见过数据之后又断了 3s */
        if (have_any && (now - last_update) >= HR_MEASURE_STALL_MS) {
            hr_report_fail(HR_MSG_WEAK, "stalled (no update for 3s)");
            goto out;
        }

        pfd.revents = 0;
        rc = poll(&pfd, 1, 100);
        if (rc <= 0) {
            continue;
        }

        {
            struct algo_heartrate hr;
            uint32_t stamp;

            memset(&hr, 0, sizeof(hr));
            if (orb_copy(ORB_ID(algo_heartrate), fd, &hr) != 0) {
                continue;
            }

            stamp = hr_now_ms();
            last_update = stamp;
            have_any = true;
            s_last_bpm = hr.bpm;   /* 缓存最后一次成功 copy 的值 */

            if (hr.bpm == 0 ||
                hr.bpm < HR_BPM_SANE_MIN || hr.bpm > HR_BPM_SANE_MAX) {
                first_bpm = 0;
                continue;
            }

            if (first_bpm == 0) {
                first_bpm = hr.bpm;
                first_ms = stamp;
                printf("[hr_measure] candidate bpm=%u at %ums\n",
                       (unsigned)first_bpm, (unsigned)(stamp - t0));
                continue;
            }

            if (!hr_same_scale(hr.bpm, first_bpm)) {
                /* 值跳变：重新计时，等下一条确认 */
                first_bpm = hr.bpm;
                first_ms = stamp;
                continue;
            }

            /* 新鲜度确认：同一量级的值在 3s 内再次到达 */
            if ((stamp - first_ms) <= HR_MEASURE_STALL_MS &&
                (stamp - t0) >= HR_MEASURE_MIN_MS) {
                hr_report_ok(first_bpm);
                goto out;
            }
        }
    }

out:
    /* ── 3) 无论成败都停采集：SHDN 关 LED + 采集线程 close(fd) 释放总线 ──
     * （P1a 的退出路径在采集线程自己身上，这里只负责把 stop 请求送进去） */
    if (fd >= 0) {
        orb_unsubscribe(fd);
        fd = -1;
    }
    if (started) {
        exitcode = 0;
        rc = hr_sensor_cmd("hr_stop", "stop", HR_STOP_WAIT_MS, &exitcode);
        printf("[hr_measure] hr_sensor stop rc=%d exit=%d\n", rc, exitcode);
        /* 给采集线程一点时间走完 SHDN + close，也把 inited 清零留给下一轮 */
        usleep(HR_STOP_SETTLE_MS * 1000);
    }

    s_state = HR_ST_IDLE;
    pthread_mutex_lock(&s_lock);
    s_busy = false;
    pthread_mutex_unlock(&s_lock);
}

static void *hr_measure_thread(void *arg)
{
    (void)arg;

    hr_measure_worker();

    /* 兜底：worker 若从非正常路径返回，也要把单飞标志放掉 */
    s_state = HR_ST_IDLE;
    pthread_mutex_lock(&s_lock);
    s_busy = false;
    pthread_mutex_unlock(&s_lock);
    return NULL;
}

/****************************************************************************
 * Public API
 ****************************************************************************/

int hr_measure_start(void)
{
    pthread_attr_t attr;
    pthread_t tid;
    int ret;

    pthread_mutex_lock(&s_lock);
    if (s_busy) {
        pthread_mutex_unlock(&s_lock);
        printf("[hr_measure] already measuring, click ignored\n");
        return -EBUSY;
    }
    s_busy = true;
    pthread_mutex_unlock(&s_lock);

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, HR_MEASURE_STACK);
    /* detach：worker 自己收尾，调用方（LVGL render 线程）绝不等它 */
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

    ret = pthread_create(&tid, &attr, hr_measure_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        printf("[hr_measure] pthread_create failed: %d\n", ret);
        pthread_mutex_lock(&s_lock);
        s_busy = false;
        pthread_mutex_unlock(&s_lock);
        return -ENOMEM;
    }

    printf("[hr_measure] measurement started (max %ds)\n", HR_MEASURE_MAX_S);
    return 0;
}

#else /* 没有 uORB / fitness 话题：给个明确失败的桩，保证 UI 侧能链接 */

int hr_measure_start(void)
{
    printf("[hr_measure] algo_heartrate topic not available\n");
    return -ENOSYS;
}

#endif /* HR_MEASURE_HAVE_TOPIC */

