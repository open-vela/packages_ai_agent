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
 *  5) **静默模式**（hr_measure_start_silent，后台守护 hr_watch 用）：
 *     与按钮路径共用同一套 worker/采集/质量判据/收尾，仅跳过全部
 *     UI 与语音 —— 不上「测量中」进度、结果不上屏、不进 TTS、失败
 *     也不弹错误文案。后台周期测量不能打扰用户；所有 printf 探测
 *     日志保留（串口排障仍可复盘）。
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
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "health/hr_measure.h"

#include "cJSON.h"
#include "infra/health_store.h"

#if defined(CONFIG_UORB) && defined(CONFIG_FITNESS_ALGO)
#  if __has_include(<fitness/algo_heartrate.h>)
#    define HR_MEASURE_HAVE_TOPIC 1
#  endif
#endif

/****************************************************************************
 * 心率历史（持久化环形缓冲）
 *
 * 288 条 ≈ 后台 hr_watch 每 10 分钟一轮的两天数据。每次测量成功
 * （手动 / 静默两条路径都经过 hr_report_ok）追加一条并立即落盘：
 *
 *   {"items":[{"t":1730000000,"bpm":88},{"t":...}]}
 *
 * 失败语义全部是「从空开始 / 不崩」：
 *  - 文件不存在、读失败、JSON 损坏、字段类型不对：跳过该条或整体当空；
 *  - 保存失败（/data 还没挂好、盘满）：只告警。RAM 环照常可用并继续
 *    累积，下一次成功保存会把整段历史补写进去 —— 一次写盘失败不会
 *    丢掉历史。
 *
 * 锁：s_hist_lock 保护环形缓冲和 s_hist_loaded。
 *   load：整个「读文件 + cJSON 解析」在锁内完成，其它线程等它读完，
 *     不会看到半截数据；只在首次 start 或首次查询时做一次。
 *   save：组装 JSON 在锁内，**写文件在锁外** —— littlefs 写入可能几十
 *     毫秒，不能把 render 线程的 snapshot 堵在盘上。
 ****************************************************************************/

#define HR_HISTORY_FILE "hr_history"

static pthread_mutex_t s_hist_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_hist_ts[HR_MEASURE_HISTORY_CAP];
static int      s_hist_bpm[HR_MEASURE_HISTORY_CAP];
static int      s_hist_next;    /* 下一个写入位置 */
static int      s_hist_count;   /* 有效条数 0..HR_MEASURE_HISTORY_CAP */
static bool     s_hist_loaded;  /* 是否已尝试过读盘（失败也只试一次） */

/* 逻辑下标（0 = 最旧）-> 物理下标。调用者必须持有 s_hist_lock。 */
static int hr_hist_slot(int i)
{
    if (s_hist_count < HR_MEASURE_HISTORY_CAP) {
        return i;   /* 还没绕过一圈，物理顺序就是时间顺序 */
    }
    return (s_hist_next + i) % HR_MEASURE_HISTORY_CAP;
}

/* 追加一条（锁内调用）。满了就覆盖最旧的一条。 */
static void hr_hist_push(uint32_t t, int bpm)
{
    s_hist_ts[s_hist_next] = t;
    s_hist_bpm[s_hist_next] = bpm;
    s_hist_next = (s_hist_next + 1) % HR_MEASURE_HISTORY_CAP;
    if (s_hist_count < HR_MEASURE_HISTORY_CAP) {
        s_hist_count++;
    }
}

/* 读盘并重建环形缓冲。调用者必须持有 s_hist_lock。 */
static void hr_hist_load(void)
{
    char *buf;
    cJSON *root;
    cJSON *items;
    cJSON *it;
    int n;

    s_hist_loaded = true;   /* 只尝试一次：/data 未就绪也不反复读盘 */

    buf = (char *)malloc(HR_MEASURE_HISTORY_JSON_MAX);
    if (buf == NULL) {
        printf("[hr_measure] history load n=0 (no memory)\n");
        return;
    }

    n = health_store_load(HR_HISTORY_FILE, buf, HR_MEASURE_HISTORY_JSON_MAX);
    if (n < 0) {
        printf("[hr_measure] history load n=0 (no file rc=%d)\n", n);
        free(buf);
        return;
    }
    if (n >= HR_MEASURE_HISTORY_JSON_MAX - 1) {
        printf("[hr_measure] history load n=0 (file too large)\n");
        free(buf);
        return;
    }

    root = cJSON_Parse(buf);
    free(buf);
    if (root == NULL) {
        printf("[hr_measure] history load n=0 (bad json)\n");
        return;
    }

    items = cJSON_GetObjectItemCaseSensitive(root, "items");
    if (cJSON_IsArray(items)) {
        cJSON_ArrayForEach(it, items) {
            cJSON *jt = cJSON_GetObjectItemCaseSensitive(it, "t");
            cJSON *jb = cJSON_GetObjectItemCaseSensitive(it, "bpm");
            int bpm;

            if (!cJSON_IsNumber(jt) || !cJSON_IsNumber(jb)) {
                continue;
            }

            bpm = (int)cJSON_GetNumberValue(jb);
            if (bpm <= 0 || bpm > 300) {
                continue;   /* 垃圾值不进曲线，也不进工具输出 */
            }

            hr_hist_push((uint32_t)cJSON_GetNumberValue(jt), bpm);
        }
    }

    cJSON_Delete(root);
    printf("[hr_measure] history load n=%d\n", s_hist_count);
}

/* 首次使用时 load 一次。任何线程可调。 */
static void hr_hist_ensure_loaded(void)
{
    pthread_mutex_lock(&s_hist_lock);
    if (!s_hist_loaded) {
        hr_hist_load();
    }
    pthread_mutex_unlock(&s_hist_lock);
}

#ifdef HR_MEASURE_HAVE_TOPIC

/* 组装完整历史并落盘。返回 0 成功，负数失败（RAM 数据不受影响）。
 * 只有测量成功收尾会调它（两条路径共用 hr_report_ok），低频。 */
static int hr_hist_save(void)
{
    cJSON *root;
    cJSON *items;
    char *json;
    int i;
    int ret = 0;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return -ENOMEM;
    }

    items = cJSON_AddArrayToObject(root, "items");
    if (items == NULL) {
        cJSON_Delete(root);
        return -ENOMEM;
    }

    pthread_mutex_lock(&s_hist_lock);
    for (i = 0; i < s_hist_count; i++) {
        int slot = hr_hist_slot(i);
        cJSON *obj = cJSON_CreateObject();

        if (obj == NULL) {
            ret = -ENOMEM;
            break;
        }

        cJSON_AddNumberToObject(obj, "t", (double)s_hist_ts[slot]);
        cJSON_AddNumberToObject(obj, "bpm", (double)s_hist_bpm[slot]);
        cJSON_AddItemToArray(items, obj);
    }
    pthread_mutex_unlock(&s_hist_lock);

    if (ret == 0) {
        json = cJSON_PrintUnformatted(root);
        if (json == NULL) {
            ret = -ENOMEM;
        } else {
            ret = health_store_save(HR_HISTORY_FILE, json);
            free(json);
        }
    }

    cJSON_Delete(root);

    if (ret != 0) {
        printf("[hr_measure] history save failed: %d\n", ret);
    }
    return ret;
}

/* 追加一条并立即落盘（测量成功收尾调用，低频：后台 10 分钟一次）。 */
static void hr_hist_append(uint32_t t, int bpm)
{
    int n;
    int ret;

    /* 必须先 load：否则第一次保存会用「只有新条目」的 RAM 环覆盖掉
     * 磁盘上的旧历史。 */
    hr_hist_ensure_loaded();

    pthread_mutex_lock(&s_hist_lock);
    hr_hist_push(t, bpm);
    n = s_hist_count;
    pthread_mutex_unlock(&s_hist_lock);

    ret = hr_hist_save();

    printf("[hr_measure] history append t=%u bpm=%d n=%d save=%d\n",
           (unsigned)t, bpm, n, ret);
}

#endif /* HR_MEASURE_HAVE_TOPIC */

/****************************************************************************
 * 历史 Public API（无 uORB 时也要能链接，所以放在 topic 守卫外面）
 ****************************************************************************/

int hr_measure_history_snapshot(uint32_t *ts, int *bpm, int max)
{
    int i;
    int n;
    int start;

    if (ts == NULL || bpm == NULL) {
        return -EINVAL;
    }
    if (max <= 0) {
        return 0;
    }

    hr_hist_ensure_loaded();

    pthread_mutex_lock(&s_hist_lock);
    n = (s_hist_count < max) ? s_hist_count : max;
    start = s_hist_count - n;   /* 取最近 n 条（旧 -> 新） */
    for (i = 0; i < n; i++) {
        int slot = hr_hist_slot(start + i);

        ts[i] = s_hist_ts[slot];
        bpm[i] = s_hist_bpm[slot];
    }
    pthread_mutex_unlock(&s_hist_lock);

    return n;
}

int hr_measure_history_json(char *out, size_t cap)
{
    cJSON *root;
    cJSON *items;
    char *json;
    int i;
    int ret = 0;

    if (out == NULL || cap == 0) {
        return -EINVAL;
    }
    out[0] = '\0';

    hr_hist_ensure_loaded();

    root = cJSON_CreateObject();
    if (root == NULL) {
        return -ENOMEM;
    }

    items = cJSON_AddArrayToObject(root, "items");
    if (items == NULL) {
        cJSON_Delete(root);
        return -ENOMEM;
    }

    pthread_mutex_lock(&s_hist_lock);
    for (i = 0; i < s_hist_count; i++) {
        int slot = hr_hist_slot(i);
        cJSON *obj = cJSON_CreateObject();

        if (obj == NULL) {
            ret = -ENOMEM;
            break;
        }

        cJSON_AddNumberToObject(obj, "t", (double)s_hist_ts[slot]);
        cJSON_AddNumberToObject(obj, "bpm", (double)s_hist_bpm[slot]);
        cJSON_AddItemToArray(items, obj);
    }
    pthread_mutex_unlock(&s_hist_lock);

    if (ret == 0) {
        json = cJSON_PrintUnformatted(root);
        if (json == NULL) {
            ret = -ENOMEM;
        } else {
            size_t len = strlen(json);

            if (len >= cap) {
                ret = -ENOSPC;
            } else {
                memcpy(out, json, len + 1);
            }
            free(json);
        }
    }

    cJSON_Delete(root);
    return ret;
}


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

/* max30102.c 暴露的采集状态查询/中止入口（另一个任务组里的 static 状态，
 * 只能通过这两个函数访问）。用途见下面 start 失败路径的注释。 */
extern void max30102_hr_abort(void);
extern int  max30102_hr_running(void);

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

/* 结果被接受前的最短测量窗口。
 *
 * 2026-10 从 4s 提到 15s: 24.13 真机实测连测三次得 143 / 136 / 85 bpm,
 * 而算法侧 pk5s=6 rr_med=700ms 明明指向 85 —— 前两次是手指刚接触时的暂态。
 * 4s 窗口在算法还没收敛时就放行, 于是把暂态报给了用户。
 * 上限 HR_MEASURE_MAX_S = 30s, 所以 15s 仍留出一半余量给"手指刚放上去 /
 * 信号弱"的场合。 */
#define HR_MEASURE_MIN_MS  15000
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

/* 本轮是否为静默测量（hr_measure_start_silent 起的）。
 * worker 启动时置位、结束时不清除 —— hr_measure_start() 的按钮路径
 * 每轮开头都会把它重新写成 false，两个模式不会串。 */
static volatile bool s_silent;

/* ★ 最近一轮测量**有没有测到**（成功 = 出过被接受的 median）。
 *
 * 为什么需要它：失败轮（信号弱提前放弃 / 30s 超时）里 s_last_bpm
 * 可能残留「最后一次 copy 到的非零值」（hr_measure_worker 里
 * s_last_bpm = hr.bpm 那行 —— copy 到过但没通过质量判据）。
 * 只看 last_bpm()!=0 判断成败会把失败轮当成功轮：hr_watch 的后台
 * 分诊会拿这个残留值去触发心率告警 —— 正是「拿噪声告警」这条
 * 诚实性红线。所以把「本轮成败」显式记下来，读结果的一方
 * 先看它、再看数值。 */
static volatile bool s_last_ok;

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
    /* 只给真正的参数, 不要把程序名写进来 —— NuttX 的 nxtask_setup_stackargs()
     * (sched/task/task_setup.c:538, 由 task_init.c:134 调用) 会自己用 name 填 argv[0],
     * 并把这里传入的数组整体接在后面。写成 {name, cmd, NULL} 会得到
     * argc=3 / argv[0]=name / argv[1]=name / argv[2]=cmd, 于是 hr_sensor 的
     * `cmd = (argc > 1) ? argv[1] : "status"` 取到的是程序名, 匹配不上任何子命令,
     * 打印用法后立即退出 —— 真机表现就是: 按钮点了、倒计时在跑、但采集从未启动
     * (没有红光/没有 bpm/15s 超时)。 */
    FAR char * const argv[2] = { (FAR char *)cmd, NULL };
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
    s_last_ok = true;   /* 本轮测到了：hr_watch 据此放行数值 */
    /* 把缓存同步成**被接受的 median**（入参），而不是留最后一次原始
     * copy 值（可能与 median 差 ±15%）。这样语音念的、界面上显示的、
     * hr_watch 拿去判阈值的是同一个数 —— 三个入口一个口径。 */
    s_last_bpm = bpm;

    printf("[hr_measure] success: bpm=%u%s\n", (unsigned)bpm,
           s_silent ? " (silent)" : "");

    /* 历史落盘：手动轮和静默轮都经过这里，一次测量一条。
     * 时间用墙上时间（time(NULL)），页面/工具按它排序展示；系统时间
     * 还没同步时写 0，不编造时间戳。 */
    {
        time_t now = time(NULL);

        hr_hist_append(now > 0 ? (uint32_t)now : 0u, (int)bpm);
    }

    if (s_silent) {
        return;   /* 静默轮：不上屏、不进 TTS，只留串口日志 */
    }

    /* 一次完成：气泡 + 历史 + 桌宠表情 + 语音播报。
     * AGENT_MSG_KIND_FINAL 是唯一会出声的类别（P2a 的显示/播报解耦点）。 */
    /* 您的心率是 NN */
    snprintf(text, sizeof(text),
             "\xe6\x82\xa8\xe7\x9a\x84\xe5\xbf\x83\xe7\x8e\x87\xe6\x98\xaf"
             " %u",
             (unsigned)bpm);
    lvgl_ui_channel_send_kind(text, AGENT_MSG_KIND_FINAL);
    lvgl_ui_post(greeting_restore_async_cb, NULL);
}

static void hr_report_fail(const char *text, const char *why)
{
    s_state = HR_ST_FAILED;
    s_last_ok = false;  /* 本轮没测到：残留的 s_last_bpm 不算数 */

    printf("[hr_measure] failed: %s (last bpm=%u)%s\n",
           why, (unsigned)s_last_bpm, s_silent ? " (silent)" : "");

    if (s_silent) {
        return;   /* 静默轮：不弹错误文案打扰用户，hr_watch 只计失败 */
    }
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
/* ── 滑动窗口中位数 ──────────────────────────────────────────────
 *
 * 为什么不再用"第一个候选值"上报: 24.13 真机连测三次得到 143 / 136 / 85,
 * 算法侧的 pk5s=6 rr_med=700ms 指向 85 —— 143/136 是手指刚接触时的暂态。
 * 旧逻辑 hr_report_ok(first_bpm) 恰好把这个暂态报了出去。
 *
 * 改成: 每个通过 sanity 的值推进一个长度 HR_RECENT_N 的窗口, 跑满最短窗口
 * 之后取**中位数**上报。中位数对单个异常值免疫, 而接触暂态只会出现在窗口
 * 前段, 会被后续的真值稀释掉。
 */
#define HR_RECENT_N 7
static uint32_t s_recent[HR_RECENT_N];
static int      s_recent_cnt;
static int      s_recent_pos;

static void hr_recent_reset(void)
{
    memset(s_recent, 0, sizeof(s_recent));
    s_recent_cnt = 0;
    s_recent_pos = 0;
}

static void hr_recent_push(uint32_t bpm)
{
    s_recent[s_recent_pos] = bpm;
    s_recent_pos = (s_recent_pos + 1) % HR_RECENT_N;
    if (s_recent_cnt < HR_RECENT_N) {
        s_recent_cnt++;
    }
}

static uint32_t hr_recent_median(void)
{
    uint32_t v[HR_RECENT_N];
    int i, j;

    if (s_recent_cnt <= 0) {
        return 0;
    }

    memcpy(v, s_recent, sizeof(uint32_t) * (size_t)s_recent_cnt);

    /* 插入排序, 最多 7 个元素 */
    for (i = 1; i < s_recent_cnt; i++) {
        uint32_t key = v[i];
        j = i - 1;
        while (j >= 0 && v[j] > key) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = key;
    }

    return v[s_recent_cnt / 2];
}

static void hr_measure_worker(void)
{
    struct pollfd pfd;
    uint32_t t0;
    uint32_t last_update = 0;
    int last_left = -1;
    int fd = -1;
    int exitcode = 0;
    int rc;
    bool have_any = false;
    bool started = false;

    s_state = HR_ST_STARTING;

    /* 首次测量（手动 / 静默都走这里）把持久化历史装进 RAM。放在 worker
     * 线程而不是 hr_spawn：读盘 + JSON 解析不能卡 render 线程的点击回调。 */
    hr_hist_ensure_loaded();

    /* 只统计本轮: s_last_bpm 是文件级 static, 不清的话失败信息里的
     * "last bpm=" 会带上**上一轮的旧值**, 真机排查时被它误导过
     * (看到 last bpm=171 以为本轮收到过数据, 其实一次都没收到)。 */
    s_last_bpm = 0;
    s_last_ok = false;   /* 本轮成败从零开始，结束时由 report_ok/fail 落笔 */
    hr_recent_reset();

    /* ── 1) 起采集（既有 CLI 路径） ─────────────────────────────── */
    rc = hr_sensor_cmd("hr_start", "start", HR_START_WAIT_MS, &exitcode);
    printf("[hr_measure] hr_sensor start rc=%d exit=%d\n", rc, exitcode);

    /* exitcode == -1 只表示「没拿到退出码」，不当作失败：hr_sensor start 若
     * 已经成功起好了采集，误报「未接传感器」比多等几秒更糟，而且后面还会
     * 因为拿不到数据被 15s 上限兜住。只有**明确非零**的退出码才提前判定
     * 传感器没接上（hr_start 打不开总线/初始化失败时就是这条路）。 */
    if (rc != 0 || exitcode > 0) {
        /* start 失败/超时后的**真实回收**（对抗审查 2026-10-19）。
         *
         * 原来这里是直接 goto out —— 而 out 里只在 started==true 时才发 stop，
         * 所以"被 spawn 的采集线程稍后才起来"这种情况就彻底没人管了。
         *
         * 但"补发一个 stop 就算清理"也不够：start 超时最常见的原因是采集进程
         * 还卡在 max30102_init 里，此时 g_hr.running 还是 false，stop 走过去
         * 只会打印"未运行"立即返回。真正有效的三步：
         *   1) 置 abort 标志，让那个进程在 task_create 之前自己退出；
         *   2) 仍然发一次 stop，覆盖"采样线程已经起来"的那种情况；
         *   3) 轮询 running 直到 false —— 只有它真的为 false 才算清理干净。
         */
        int tries;

        max30102_hr_abort();
        exitcode = 0;
        (void)hr_sensor_cmd("hr_stop", "stop", HR_STOP_WAIT_MS, &exitcode);
        for (tries = 0; tries < 40 && max30102_hr_running() != 0; tries++) {
            usleep(50 * 1000);
        }
        printf("[hr_measure] start failed, reclaim: running=%d after %dms\n",
               max30102_hr_running(), tries * 50);
        started = false;
        hr_report_fail(HR_MSG_NO_SENSOR, "start failed");
        goto out;
    }
    started = true;

    /* ── 2) 订阅（在 start 之后：话题由采集线程 advertise） ────────
     *
     * ★ 先 unlink 掉上一轮遗留的主题节点(2026-10-13 真机定位的根因):
     *   NuttX 的 uORB 仿真是 sensor 框架, /dev/uorb/<name><inst> 只注册一次、
     *   **不会被自动删除**; 而 SNIOC_SET_BUFFER_NUMBER 只在
     *   !circbuf_is_init(&upper->buffer) 时才生效
     *   (nuttx/drivers/sensors/sensor.c:1054), 也就是第一个 advertiser 之后
     *   该节点的环形缓冲配置就固化了。
     *   真机症状: **只有开机后第一次测心率能出数**, 之后每次都是
     *   "[hr_measure] failed: timeout, no data (last bpm=<上一轮的旧值>)",
     *   而同一时刻算法侧明明 gate=ok q=100 bpm=83 并且在逐秒发布。
     *   unlink 之后 orb_subscribe 会重新 SNIOC_REGISTER 一个全新节点, 采集线程
     *   随后的 advertise 再 open() 复用同一节点 —— 与"第一次"路径完全一致。
     * 失败无所谓(节点本来就不存在时会返回错误), 所以忽略返回值。 */
    (void)orb_unlink(ORB_ID(algo_heartrate));

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

        /* 每秒刷一次进度（走 lvgl_ui_post，不写历史环）。
         * 静默轮不上进度 —— 后台测量对用户完全不可见。 */
        left = (int)(HR_MEASURE_MAX_S - elapsed / 1000u);
        if (left < 0) {
            left = 0;
        }
        if (!s_silent && left != last_left) {
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
                /* 不推进窗口: 越界值(算法没收敛/无数据)不能污染中位数。 */
                continue;
            }

            /* 推进滑动窗口。不再用"第一个候选值"上报 —— 见 hr_recent_median
             * 上方的注释: 那个值在真机上就是手指刚接触时的暂态(143)。 */
            hr_recent_push(hr.bpm);

            if (s_recent_cnt >= HR_RECENT_N &&
                (stamp - t0) >= HR_MEASURE_MIN_MS) {
                uint32_t med = hr_recent_median();

                /* 中位数本身要合理, 且与最新值同量级 —— 证明算法仍锁着、
                 * 没有在窗口末尾跳走(跳走说明这段数据不可信, 继续等)。 */
                if (med >= HR_BPM_SANE_MIN && med <= HR_BPM_SANE_MAX &&
                    hr_same_scale(med, hr.bpm)) {
                    printf("[hr_measure] accept median=%u latest=%u n=%d at %ums\n",
                           (unsigned)med, (unsigned)hr.bpm, s_recent_cnt,
                           (unsigned)(stamp - t0));
                    hr_report_ok(med);
                    goto out;
                }

                printf("[hr_measure] hold: median=%u latest=%u n=%d at %ums\n",
                       (unsigned)med, (unsigned)hr.bpm, s_recent_cnt,
                       (unsigned)(stamp - t0));
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

/* 公共起测实现：silent=true 走后台静默轮（hr_watch 用），两个入口
 * 共用同一套单飞互斥与线程参数，只差 s_silent 一个标志。 */
static int hr_spawn(bool silent)
{
    pthread_attr_t attr;
    pthread_t tid;
    int ret;

    pthread_mutex_lock(&s_lock);
    if (s_busy) {
        pthread_mutex_unlock(&s_lock);
        printf("[hr_measure] already measuring, %s ignored\n",
               silent ? "silent start" : "click");
        return -EBUSY;
    }
    s_busy = true;
    s_silent = silent;   /* 锁内写：worker 起跑前就能看到本轮模式 */
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

    printf("[hr_measure] measurement started (max %ds, silent=%d)\n",
           HR_MEASURE_MAX_S, (int)silent);
    return 0;
}

int hr_measure_start(void)
{
    return hr_spawn(false);
}

int hr_measure_start_silent(void)
{
    return hr_spawn(true);
}

bool hr_measure_last_ok(void)
{
    return s_last_ok;
}

int hr_measure_last_bpm(void)
{
    return (int)s_last_bpm;
}

bool hr_measure_is_running(void)
{
    bool busy;

    pthread_mutex_lock(&s_lock);
    busy = s_busy;
    pthread_mutex_unlock(&s_lock);

    return busy;
}

#else /* 没有 uORB / fitness 话题：给个明确失败的桩，保证 UI 侧能链接 */

int hr_measure_start(void)
{
    printf("[hr_measure] algo_heartrate topic not available\n");
    return -ENOSYS;
}

int hr_measure_start_silent(void)
{
    /* 后台静默轮同样没有数据源；返回失败让 hr_watch 计失败次数 */
    printf("[hr_measure] algo_heartrate topic not available (silent)\n");
    return -ENOSYS;
}

bool hr_measure_last_ok(void)
{
    return false;
}

int hr_measure_last_bpm(void)
{
    return 0;
}

bool hr_measure_is_running(void)
{
    return false;
}

#endif /* HR_MEASURE_HAVE_TOPIC */

