/****************************************************************************
 * pet_care.c - Proactive companion scheduler implementation.
 *
 * 所有主动开口都汇到 care_fire()：L1 模板气泡（lvgl_ui_channel_send，
 * 复用情绪推断与气泡渲染）+ 历史环（lvgl_ui_channel_log，无气泡通道）。
 * L2 增强走 message_bus 的 "care" 通道进 agent loop —— 云端 LLM → 本地
 * 模型兜底的整条链路都是现成的，回复从 outbound "care" 分支渲染成
 * 第二条气泡。L1 已经保底，L2 失败完全静默。
 *
 * 为什么不用独立线程：SRAM 余量紧张（docs_ble/24），且 agent_loop、
 * render 线程都在 —— tick 挂 render 线程 1Hz 分频零额外成本。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "pet_care.h"
#include "lvgl_ui_channel.h"
#include "pet_display.h"
#include "core/message_bus.h"
#include "health/hr_monitor.h"

#define TAG "pet_care"

/* render 线程 5ms 一拍，200 拍 = 1 秒 */
#define TICKS_PER_SEC   200   /* render 5ms 一拍，200 拍 = 1s */

/* "care" 通道名（outbound dispatch 需识别同名字符串） */
#define CARE_CHANNEL     "care"
#define CARE_CHAT_ID     "pet_care"

/* 现在时刻（秒，MONOTONIC —— 不受对时影响，空闲/闹钟计时都用它） */
static time_t care_now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
}

/* 本地时刻的「时」。板上策略是 RTC 直接存本地时间（LOG Round 20/21），
 * 所以 gmtime 即本地时。拿不到就当正午（不静默也不深夜）。 */
static int care_local_hour(void)
{
    time_t now = time(NULL);
    struct tm tm_buf;
    if (gmtime_r(&now, &tm_buf) != NULL) {
        return tm_buf.tm_hour;
    }
    return 12;
}

/* 静默期：22:00（含）～ 次日 07:00（不含） */
static bool care_is_quiet(void)
{
    int h = care_local_hour();
    return h >= PET_CARE_QUIET_START_DEFAULT || h < PET_CARE_QUIET_END_DEFAULT;
}

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* 闹钟状态机阶段 */
enum alarm_stage_e {
    ALARM_OFF = 0,     /* 无闹钟 */
    ALARM_SET,         /* 已设定，等 T-60s */
    ALARM_PRE,         /* 已播预告，等 T0 */
    ALARM_FIRE,        /* 已播晨间简报，等确认或升级 */
    ALARM_ESC,         /* 已升级催促，等确认 */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* L1 模板库：时段（早/中/晚）× 3 条。文案留在 ui_font_cjk_18 子集内
 * （与 KEY2 演示问句同一约束）。 */
static const char *g_idle_templates[][3] = {
    { "早上好呀～喝杯水再忙哦",
      "新的一天，我在这儿陪你",
      "记得吃早餐，不然我会担心的" },
    { "中午啦，别一直盯着屏幕呀",
      "休息一下眼睛吧，眨眨～",
      "要不要和我说说话？" },
    { "晚上好～今天过得怎么样？",
      "累了一天，放松一下吧",
      "我在呢，想聊点什么吗？" },
};

static const char *g_alarm_pre_templates[] = {
    "还有一分钟就到时间啦，准备好哦～"
};

static const char *g_alarm_fire_templates[] = {
    "时间到啦！早上好，新的一天开始咯",
    "起床啦起床啦～我都等急了",
    "叮～到点了哦，看看今天的安排吧"
};

static const char *g_alarm_esc_templates[] = {
    "嘿，还没动呢？我可要挠你痒痒了！",
    "再不起就要迟到咯，快起来嘛～"
};

static const char *g_hr_alert_templates[] = {
    "刚刚测到心跳有点快，先坐下歇会儿吧",
    "心跳有点快哦，深呼吸放松一下～"
};

static time_t s_last_activity_s;      /* 最近一次触摸/按键 */
static time_t s_last_care_s;          /* 最近一次主动开口（含告警） */
static int    s_tick_div;             /* 5ms → 1s 分频计数 */
static bool   s_inited;
static int    s_alarm_minutes;        /* 设定的分钟数 */
static time_t s_alarm_at;             /* 闹钟到点时刻（MONOTONIC 秒） */
static enum alarm_stage_e s_alarm_stage;
static time_t s_alarm_fire_s;         /* T0 播报时刻（升级判定基准） */
static time_t s_last_hr_alert_s;     /* 心率告警冷却基准 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* 时段索引：早（5-11）中（11-18）晚（其他） */
static int care_daypart(void)
{
    int h = care_local_hour();
    if (h >= 5 && h < 11) {
        return 0;
    }
    if (h >= 11 && h < 18) {
        return 1;
    }
    return 2;
}

static const char *care_pick(const char *const *arr, int n)
{
    return arr[(unsigned)rand() % (unsigned)n];
}

/* L1 主动开口：气泡（情绪随文案自动推断）+ 历史环记录。
 * quiet=true 时只进历史不上屏（不打扰睡眠，演示日志仍可复盘）。 */
static void care_fire(const char *text, bool quiet)
{
    if (text == NULL) {
        return;
    }

    s_last_care_s = care_now_s();

    if (!quiet) {
        lvgl_ui_channel_send(text);
    }
    lvgl_ui_channel_log(text, false);
}

/* L2 增强：把关怀上下文推给 agent loop（云端→本地兜底整条链路）。
 * 失败静默——L1 气泡已经出去了。 */
static void care_start_l2(const char *l1_text)
{
    agent_msg_t msg = { 0 };
    char prompt[160];

    snprintf(prompt, sizeof(prompt),
             "主动关怀：你刚对用户说了「%s」，请自然地接着这句关心一下用户的近况。",
             l1_text);

    strncpy(msg.channel, CARE_CHANNEL, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, CARE_CHAT_ID, sizeof(msg.chat_id) - 1);
    msg.content = strdup(prompt);
    if (msg.content == NULL) {
        return;
    }

    if (message_bus_push_inbound(&msg) != 0) {
        syslog(LOG_WARNING, "[%s] inbound queue rejected care prompt\n", TAG);
        free(msg.content);
    }
}

/* 场景②：空闲关怀。闹钟 pending 时不开口（避免和叫醒序列互相轰炸）；
 * 冷却期内不开口；静默期不开口。 */
static void care_idle_check(void)
{
    time_t now = care_now_s();
    time_t idle_ref = s_last_activity_s > s_last_care_s ? s_last_activity_s
                                                       : s_last_care_s;
    const char *text;
    int part;

    if (s_alarm_stage != ALARM_OFF) {
        return;
    }
    if (now - idle_ref < (time_t)PET_CARE_IDLE_MIN_DEFAULT * 60) {
        return;
    }
    if (now - s_last_care_s < (time_t)PET_CARE_COOLDOWN_MIN_DEFAULT * 60) {
        return;
    }
    if (care_is_quiet()) {
        return;
    }

    part = care_daypart();
    text = care_pick(g_idle_templates[part], 3);
    care_fire(text, false);
    care_start_l2(text);
}

/* 场景①：闹钟状态机（每秒一拍；刻意穿透静默期——叫醒本来就该响） */
static void care_alarm_check(void)
{
    time_t now = care_now_s();

    switch (s_alarm_stage) {
    case ALARM_SET:
        if (now + 60 >= s_alarm_at) {
            care_fire(care_pick(g_alarm_pre_templates, 1), false);
            s_alarm_stage = ALARM_PRE;
        }
        break;

    case ALARM_PRE:
        if (now >= s_alarm_at) {
            care_fire(care_pick(g_alarm_fire_templates, 3), false);
            s_alarm_fire_s = now;
            s_alarm_stage = ALARM_FIRE;
        }
        break;

    case ALARM_FIRE:
    case ALARM_ESC:
        /* 到点后 ESC 秒无活动且期间没碰过设备 → 升级一次（只升一级） */
        if (s_alarm_stage == ALARM_FIRE &&
            now - s_alarm_fire_s >= PET_CARE_ALARM_ESC_SEC &&
            s_last_activity_s < s_alarm_fire_s) {
            care_fire(care_pick(g_alarm_esc_templates, 2), false);
            s_alarm_stage = ALARM_ESC;
        }
        break;

    default:
        break;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int pet_care_init(void)
{
    if (s_inited) {
        return 0;
    }

    s_last_activity_s = care_now_s();   /* 开机视为一次活动，从满 idle 窗起算 */
    s_last_care_s = 0;
    s_tick_div = 0;
    s_alarm_stage = ALARM_OFF;
    s_alarm_minutes = 0;
    s_last_hr_alert_s = 0;

    s_inited = true;
    syslog(LOG_INFO, "[%s] init ok (idle=%dmin cooldown=%dmin quiet=%d-%d)\n",
           TAG, PET_CARE_IDLE_MIN_DEFAULT, PET_CARE_COOLDOWN_MIN_DEFAULT,
           PET_CARE_QUIET_START_DEFAULT, PET_CARE_QUIET_END_DEFAULT);
    return 0;
}

void pet_care_tick(void)
{
    if (!s_inited) {
        return;
    }

    if (++s_tick_div < TICKS_PER_SEC) {
        return;
    }
    s_tick_div = 0;

    /* 1Hz 逻辑 */
    care_alarm_check();
    care_idle_check();
    hr_monitor_tick();   /* 场景③：模拟心率步进 + 阈值判定 */
}

void pet_care_note_activity(void)
{
    s_last_activity_s = care_now_s();

    /* 闹钟 pending 时，任何触摸/按键都视为唤醒确认：记录并清闹钟 */
    if (s_alarm_stage == ALARM_FIRE || s_alarm_stage == ALARM_ESC) {
        char buf[32];
        time_t now = time(NULL);
        struct tm tm_buf;
        if (gmtime_r(&now, &tm_buf) != NULL) {
            snprintf(buf, sizeof(buf), "[唤醒确认] %02d:%02d",
                     tm_buf.tm_hour, tm_buf.tm_min);
            lvgl_ui_channel_log(buf, true);
        }
        syslog(LOG_INFO, "[%s] alarm acked by activity\n", TAG);
        pet_care_alarm_cancel();
    }
}

int pet_care_alarm_set(int minutes)
{
    if (minutes <= 0 || minutes > 24 * 60) {
        return -1;
    }

    s_alarm_minutes = minutes;
    s_alarm_at = care_now_s() + (time_t)minutes * 60;
    s_alarm_stage = ALARM_SET;
    syslog(LOG_INFO, "[%s] alarm set %dmin\n", TAG, minutes);
    return 0;
}

void pet_care_alarm_cancel(void)
{
    if (s_alarm_stage != ALARM_OFF) {
        syslog(LOG_INFO, "[%s] alarm cancelled (was stage %d, %dmin)\n",
               TAG, (int)s_alarm_stage, s_alarm_minutes);
    }
    s_alarm_stage = ALARM_OFF;
    s_alarm_minutes = 0;
}

void pet_care_hr_report(int bpm)
{
    time_t now = care_now_s();

    /* 告警冷却：持续高值只提醒一次，避免连环轰炸 */
    if (now - s_last_hr_alert_s < (time_t)PET_CARE_COOLDOWN_MIN_DEFAULT * 60) {
        return;
    }

    s_last_hr_alert_s = now;
    care_fire(care_pick(g_hr_alert_templates, 2), false);

    {
        char buf[48];
        snprintf(buf, sizeof(buf), "[心率告警] %d bpm", bpm);
        lvgl_ui_channel_log(buf, true);
    }
    syslog(LOG_INFO, "[%s] hr alert %d bpm\n", TAG, bpm);
}
