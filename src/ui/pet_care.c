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
 * render 线程都在 —— tick 挂 render 线程、墙钟秒门控，零额外成本。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "pet_care.h"
#include "voice/pcm_file_player.h"   /* pcm_file_is_playing：音乐在播时不要抢设备 */
#include "lvgl_ui_channel.h"
#include "pet_display.h"
#include "core/message_bus.h"
#include "health/hr_monitor.h"
#include "health/pedometer.h"   /* 久坐提醒/步数里程碑：真 IMU 数据源 */
#include "agent_config.h"        /* DEMO-FW：AGENT_CHAN_VOICE */
#include "infra/cron_service.h"  /* DEMO-FW：演示闹钟 = 一次性 cron voice 任务 */
#include "voice/demo_voice_cache.h" /* DEMO-VOICE-A：固定台词预合成音频缓存 */
#include "core/care_router.h"       /* PROACTIVE-MVP P0：主动关怀规则 Router */
#include "core/care_ticket.h"        /* PROACTIVE-P1A：ACTION/DEFER ticket（RAM） */
#include "core/care_log.h"           /* PROACTIVE-P1A：决策/隐式反馈环形日志（RAM） */
#include "voice/voice_channel.h"     /* PROACTIVE-P1A：PTT/录音算隐式反馈活动 */
#include "voice/voice_tts_cache.h"   /* P2A：主动关怀本地语音（离线模板 PCM，仅查表命中） */

#define TAG "pet_care"

/* 1Hz 逻辑用『墙钟秒』门控，而不是数 tick：render 每圈实际耗时
 * 取决于 lv_timer_handler（实测约 25ms，不是标称的 5ms），数 tick 会
 * 让『1 秒逻辑』退化成 5 秒一次。改用 care_now_s() 的秒变化做门，
 * 与循环速率解耦。 */

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

struct care_action_s {
    const char *text;
    pet_emotion_t emotion;
    bool start_l2;
    /* P0 Router 记账场景名（idle/sedentary）；非路由场景为 NULL。
     * 真正开口后由 tick 调 care_router_note_action() 扣预算/记冷却。 */
    const char *type;
    /* P1b：本条 care_log 的 seq（care_log_add 返回）；开口后用它精确
     * 绑定 60s 隐式反馈窗口（修复 P1a 用"最后写入下标"的并发错绑）。 */
    uint32_t care_seq;
    /* 触发依据（只用于串口日志留痕；验收"主动关怀到底为什么开口"靠它）。
     * 空串表示这条不额外打印。 */
    char detail[64];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* P2A-FIX：idle 气泡改用 care_local_voice_text("idle") 的 GREET 模板宏
 * （与本地语音逐字一致，看到=听到），故原时段模板库已删除。 */

static const char *g_alarm_pre_templates[] = {
    "还有一分钟就到时间啦，准备好哦～"
};

/* P2A-FIX：闹钟到点气泡改用 care_local_voice_text("alarm") 的 ALARM
 * 模板宏（与本地语音逐字一致），原 fire 模板库已删除；升级催促仍用下面文案。 */
static const char *g_alarm_esc_templates[] = {
    "嘿，还没动呢？时间已经到啦～",
    "再不动就要错过啦，快起来嘛～"
};

/* P2A-FIX：心率告警气泡改用 care_local_voice_text("hr_alert") 的 HR 模板宏
 * （与本地语音逐字一致），原模板库已删除。 */

/* 场景④：久坐提醒。文案不写具体分钟数 —— 阈值可被 care_test 临时改，
 * 写死"45 分钟"就会和实际触发条件对不上（诚实原则）。 */
static const char *g_sedentary_templates[] = {
    "坐太久啦，起来走两步吧～",
    "站起来活动一下，腰会感谢你的",
    "好久没动啦，陪我走两步好不好？"
};

/* 场景⑤：步数里程碑。走完就不再加戏，避免整场都在庆祝。 */
static const uint32_t g_step_milestones[] = {
    500, 1000, 2000, 5000, 10000
};

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned int s_rand_state = 1; /* 模块私有 PRNG，避免并发 rand() */
static time_t s_last_activity_s;      /* 最近一次触摸/按键 */
static time_t s_last_care_s;          /* 最近一次主动开口（含告警） */
static time_t s_last_1hz_s;           /* 上次跑 1Hz 逻辑的墙钟秒 */
static bool   s_dnd;                  /* 免打扰标志（settings/BLE 可改，bool 读写原子） */
/* PROACTIVE-MVP P0（2026-11-04）：Router 输入/节流（各 4-8B RAM） */
static uint32_t s_prev_steps;         /* 1Hz 采样步数：走路也算活动 */
static uint32_t s_step_accum;         /* 累计步数：≥8 步才算走路，防单步噪声复位 idle */
static time_t   s_last_idle_req_s;    /* idle 请求节流（防 SILENT 日志刷屏） */
static time_t   s_last_sed_req_s;     /* sedentary 请求节流 */
static bool   s_inited;
static int    s_alarm_minutes;        /* 设定的分钟数 */
static time_t s_alarm_at;             /* 闹钟到点时刻（MONOTONIC 秒） */
static enum alarm_stage_e s_alarm_stage;
static time_t s_alarm_fire_s;         /* T0 播报时刻（升级判定基准） */
static time_t s_last_hr_alert_s;     /* 心率告警冷却基准 */

/* ── 场景④久坐提醒 ── */
static uint32_t s_sedentary_trigger_s = PET_CARE_SEDENTARY_MIN_DEFAULT * 60u;
static time_t   s_last_sedentary_s;   /* 上次久坐提醒（冷却基准） */
static bool     s_sedentary_armed = true; /* 静止跨阈值后只报一次，动过才重新武装 */

/* ── 场景⑤步数里程碑 ── */
static int      s_step_milestone_idx;      /* 下一个待庆祝的里程碑下标 */
static uint32_t s_step_force_target;       /* >0: care_test 强制下一次触发 */
static char     s_milestone_text[80];      /* 文本要带步数，只能在锁内填 */

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Caller holds s_lock. */
static const char *care_pick(const char *const *arr, int n)
{
    return arr[rand_r(&s_rand_state) % (unsigned)n];
}

/* L1 主动开口：气泡（情绪随文案自动推断）+ 历史环记录。
 * quiet=true 时只进历史不上屏（不打扰睡眠，演示日志仍可复盘）。 */
static void care_fire(const char *text, pet_emotion_t emotion, bool quiet)
{
    if (text == NULL) {
        return;
    }

    /* 验收/现场排障都靠串口日志：触发动作必须留痕（气泡本身不打印）。 */
    syslog(LOG_INFO, "[%s] fire emo=%d quiet=%d text=%s\n",
           TAG, (int)emotion, (int)quiet, text);

    if (!quiet) {
        /* P2A-感知增强（task-62）：改用顶层跨页角标（不切页、5s 自动消失），
         * 不再走会切页的气泡；失败只记日志，历史照常写。 */
        int badge_ret = lvgl_ui_care_badge(text);

        if (badge_ret != 0) {
            syslog(LOG_WARNING,
                   "[%s] care badge failed: %d (history only)\n",
                   TAG, badge_ret);
        }
    }
    /* task-64：历史环记录改用 care 专用 no-nav API（lvgl_ui_channel_log 在
     * 表盘/非 AI 页会 ui_nav_goto(AI 页)，会切页）；quiet 也记录。
     * 不切页、不触发 TTS。 */
    if (lvgl_ui_care_history_log(text) != 0) {
        syslog(LOG_WARNING,
               "[%s] care history log failed, badge only\n", TAG);
    }
}


/* ── P2A（docs/112 本地语音方向）：主动关怀的离线本地语音 ──────────
 *
 * 设计约束（Lead）：
 *   - 只改 pet_care.c；按 action.type 映射到 voice_tts_cache 内置模板文案
 *     （idle->GREET / sedentary->SEDENTARY / hr_alert->HR / alarm->ALARM）；
 *   - 播放前依次检查：DND/夜间门禁不变 -> 录音中 -> TTS 忙 -> cache lookup；
 *     只有命中才调 voice_channel_speak_async()（异步入队，不阻塞 render/网络线程，
 *     播放器所有权仍归既有 speak worker）；
 *   - **不启用网络 TTS/L2**：care_start_l2 仍由 PET_CARE_L2_ENABLE=0 关着；
 *     命中后由 speak worker 走 voice_tts_cache_play 播本地 PCM，不建 TTS 会话、
 *     不占每 boot TTS 额度；
 *   - 不写盘、不新增线程；-DCARE_LOCAL_VOICE_ENABLE=0 可整段回滚。
 * 失败（未命中/设备忙/录音）一律保持原行为：只出气泡/历史，不出声。 */
#ifndef CARE_LOCAL_VOICE_ENABLE
#define CARE_LOCAL_VOICE_ENABLE 1
#endif

/* 只有 Router 管理的场景才记账（预算/冷却/隐式反馈）；alarm 只做本地语音
 * 映射，不能吃 Router 预算、也不能进 care_log（否则会和触发路径统计混淆）。
 * 与 CARE_LOCAL_VOICE_ENABLE 无关：tick 的记账分支始终需要它。 */
static bool care_router_managed_type(const char *type)
{
    return type != NULL
        && (strcmp(type, "idle") == 0
            || strcmp(type, "sedentary") == 0
            || strcmp(type, "hr_alert") == 0);
}

/* action.type -> 模板文案（气泡与本地语音**共用同一份宏**：看到=听到）。
 * 与 voice_tts_cache.c g_entries[] 的 text 字段逐字一致（同一批宏），
 * voice_tts_cache_lookup 走精确匹配命中；strcmp 守卫未知类型返回 NULL。
 * 与 CARE_LOCAL_VOICE_ENABLE 无关：气泡文案在开关关闭时也要保持同一映射。 */
static const char *care_local_voice_text(const char *type)
{
    if (type == NULL) {
        return NULL;
    }
    if (strcmp(type, "idle") == 0) {
        return DEMO_VOICE_TEXT_GREET;
    }
    if (strcmp(type, "sedentary") == 0) {
        return DEMO_VOICE_TEXT_SEDENTARY;
    }
    if (strcmp(type, "hr_alert") == 0) {
        return DEMO_VOICE_TEXT_HR;
    }
    if (strcmp(type, "alarm") == 0) {
        return DEMO_VOICE_TEXT_ALARM;
    }
    /* P2B-2A（task-68）：App care_event 场景 -> 对应缓存音频（voice=1 时播） */
    if (strcmp(type, "water") == 0) {
        return DEMO_VOICE_TEXT_WATER;
    }
    if (strcmp(type, "sleep") == 0) {
        return DEMO_VOICE_TEXT_SLEEP;
    }
    if (strcmp(type, "weather") == 0) {
        return DEMO_VOICE_TEXT_WEATHER;
    }
    return NULL;
}

#if CARE_LOCAL_VOICE_ENABLE
/* 主动关怀触发后的本地语音尝试；任何门禁不通过都只留日志，不影响气泡。 */
static void care_local_voice_try(const char *type)
{
    const char *text = care_local_voice_text(type);
    const void *pcm = NULL;
    size_t bytes = 0;
    int rc;

    if (text == NULL) {
        return;     /* 未映射类型（里程碑/演示等）不播 */
    }

    /* P2B-1A（task-65）：App care_settings.voice=0（voice_off）-> 主动关怀
     * 本地语音静音，只保留顶层角标/历史（所有 care 类型含 alarm）。
     * 设置是 RAM only（默认 1=开），不影响用户 TTS/音乐/其它声音。 */
    if (!care_router_voice_enabled()) {
        syslog(LOG_INFO,
               "[%s] local voice: skip type=%s (voice_off)\n", TAG, type);
        return;
    }

    /* F1（P2A-COLLISION，task-57）：用户语音回合进行中（ASR 结束 -> LLM ->
     * TTS 门禁窗口）绝不让主动关怀插队，否则 care 会先占 speak 队列/被
     * tts_begin 阻塞，导致用户回答 TTS 延迟/顺序错乱。 */
    if (voice_channel_turn_active()) {
        syslog(LOG_INFO,
               "[%s] local voice: skip type=%s (turn_active)\n", TAG, type);
        return;
    }

    /* 门禁顺序（P2A-FIX）：
     * - idle/sedentary/hr_alert 的 DND/夜间由 Router **运行时门禁**负责，
     *   这里不再用固定 care_is_quiet() 重复判断（避免绕过运行时配置）；
     * - alarm 允许穿透 DND/夜间（闹钟是用户设定，Router 不管理它）；
     * - 录音/音乐/TTS 忙三类"安全门"对**所有类型（含 alarm）**仍然生效。 */

    /* 录音中绝不插话（AEC/回声）；PTT 优先级最高。 */
    if (voice_channel_is_recording()) {
        syslog(LOG_INFO,
               "[%s] local voice: skip type=%s (recording)\n", TAG, type);
        return;
    }

    /* 音乐播放中不抢音频设备（alarm 同样 skip；音乐停后触发会自然重试）。 */
    if (pcm_file_is_playing()) {
        syslog(LOG_INFO,
               "[%s] local voice: skip type=%s (music)\n", TAG, type);
        return;
    }

    /* 已有 TTS/缓存播放/前一句未结束：保持原行为只出气泡，避免抢设备。 */
    if (voice_channel_is_busy()) {
        syslog(LOG_INFO,
               "[%s] local voice: skip type=%s (busy)\n", TAG, type);
        return;
    }

    /* 只播模板缓存：未命中绝不发起网络 TTS/L2（离线可用，<1s 出声）。 */
    if (voice_tts_cache_lookup(text, &pcm, &bytes) != 0) {
        syslog(LOG_INFO,
               "[%s] local voice: skip type=%s (cache miss)\n", TAG, type);
        return;
    }

    /* F4：专用 care 入队 —— cache-only/低优先级，不占 TTS 额度，队列满也
     * 绝不淘汰用户回答；worker 内 miss/-EBUSY 直接丢弃，不回退网络 TTS。 */
    rc = voice_channel_speak_care_async(text);
    if (rc == 0) {
        syslog(LOG_INFO,
               "[%s] local voice: queued type=%s pcm=%zu cache_only=1 "
               "no_budget=1 text=%.32s\n",
               TAG, type, bytes, text);
    } else {
        syslog(LOG_WARNING,
               "[%s] local voice: enqueue rejected type=%s rc=%d "
               "(cache_only=1, no network fallback)\n",
               TAG, type, rc);
    }
}
#else
static void care_local_voice_try(const char *type)
{
    (void)type;     /* CARE_LOCAL_VOICE_ENABLE=0：完全回滚到"只出气泡" */
}
#endif /* CARE_LOCAL_VOICE_ENABLE */

/* ── P2B-2A（task-68，docs_ble/114）：App care_event 场景 ────────────
 * BLE RX 线程只做：白名单校验（BLE 层）+ Router 门禁 + care_log + 投递；
 * UI/角标/本地语音全部在 LVGL render 线程执行（与 demo 钩子同一约束，
 * 避免 BT 栈线程直接碰 LVGL/音频）。不新增线程：复用 lvgl_ui_post。
 *
 * 语义（docs/114）：
 *   - eye_rest：只出角标 + 历史 + care_log，**不出声**；
 *   - water/sleep/weather：角标/历史/care_log + 对应缓存本地语音；
 *     语音仍受 voice_off / PTT / 录音 / 音乐 / TTS busy 门禁（care_local_voice_try）；
 *   - DND/夜间/预算/冷却/最小间隔被 Router 拒绝 -> suppressed=1，不投递 UI。 */
#define CARE_SCENE_EYE_REST 0
#define CARE_SCENE_WATER    1
#define CARE_SCENE_SLEEP    2
#define CARE_SCENE_WEATHER  3
#define CARE_SCENE_COUNT    4

static const char *const s_care_scene_types[CARE_SCENE_COUNT] = {
    "eye_rest", "water", "sleep", "weather"
};

/* 看到=听到原则：water/sleep/weather 的角标/历史文案 == 本地语音缓存文案；
 * eye_rest 无缓存音频，用 ui_font_cjk_18 已覆盖的短句（见字体子集核对）。 */
static const char *const s_care_scene_texts[CARE_SCENE_COUNT] = {
    "休息一下眼睛吧，眨眨～",
    DEMO_VOICE_TEXT_WATER,
    DEMO_VOICE_TEXT_SLEEP,
    DEMO_VOICE_TEXT_WEATHER
};

static int care_scene_index(const char *type)
{
    int i;

    if (type == NULL) {
        return -1;
    }
    for (i = 0; i < CARE_SCENE_COUNT; i++) {
        if (strcmp(type, s_care_scene_types[i]) == 0) {
            return i;
        }
    }
    return -1;
}

typedef struct {
    int scene;
} care_scene_post_t;

/* LVGL render 线程执行（lvgl_ui_post 回调）。 */
static void care_scene_post_cb(void *arg)
{
    care_scene_post_t *p = (care_scene_post_t *)arg;
    int scene;
    const char *type;
    const char *text;

    if (p == NULL) {
        return;
    }
    scene = p->scene;
    free(p);

    if (scene < 0 || scene >= CARE_SCENE_COUNT) {
        return;
    }
    type = s_care_scene_types[scene];
    text = s_care_scene_texts[scene];

    /* 顶层角标 + 历史（都保证不切页）；care_log 已在 BLE 线程记录。 */
    (void)lvgl_ui_care_badge(text);
    (void)lvgl_ui_care_history_log(text);

    /* eye_rest 只提醒不出声；其余场景走既有本地语音门禁（voice_off/PTT/
     * 录音/音乐/busy/cache），失败只留角标。 */
    if (scene != CARE_SCENE_EYE_REST) {
        care_local_voice_try(type);
    }

    syslog(LOG_INFO, "[%s] care_event shown type=%s audio=%s\n",
           TAG, type, (scene == CARE_SCENE_EYE_REST) ? "none" : "try");
}

/* BLE RX 线程调用：白名单 -> Router 门禁 -> ticket/care_log -> 投递 render 线程。
 * 返回 0=已受理（suppressed=0）；1=被门禁静默（suppressed=1）；<0=参数错误。
 *
 * suppressed 语义（P2B-2A-FIX/task-72 明确，回包与日志统一）：
 *   - suppressed=1：Router 门禁静默（DND/夜间/每 boot 预算/同场景冷却/
 *     最小间隔），**不建 ticket**、不出角标/历史/声音，只在 care_log 记一条
 *     SILENT 便于复盘；
 *   - suppressed=0：事件已受理。此时**必建 FIRED ticket（TTL 300s）**、必写
 *     care_log、必投递角标+历史；voice_off / PTT / 录音 / 音乐 / TTS busy
 *     只会跳过本地音频（角标/历史仍出），不改变 suppressed；
 *   - 未知 type 由 BLE 层先返回 ok=0，不会到这里。 */
int pet_care_care_event(const char *type)
{
    care_eval_t ev;
    care_features_t f;
    uint8_t flags;
    uint32_t seq;
    uint16_t ticket_id;
    const char *scene_type;
    int scene;
    care_scene_post_t *post;

    scene = care_scene_index(type);
    if (scene < 0) {
        return -EINVAL;
    }
    /* care_log/care_ticket 只保存指针（不拷贝），而 type 来自 BLE RX 缓冲，
     * 会被下一条命令覆盖：统一改用静态场景字符串，保证日志/ticket 长期有效。 */
    scene_type = s_care_scene_types[scene];

    ev = care_router_request_scene(scene_type);
    care_router_get_features(&f);
    flags = (uint8_t)((f.dnd ? CARE_LOG_FLAG_DND : 0)
                      | (f.quiet ? CARE_LOG_FLAG_QUIET : 0)
                      | (f.in_call_or_busy ? CARE_LOG_FLAG_BUSY : 0));

    if (ev.decision != CARE_ACTION) {
        /* suppressed=1：只留 care_log，不建 ticket、不出角标/历史/声音。 */
        seq = care_log_add("care_event", scene_type,
                           care_router_state_name(f.state),
                           care_router_decision_name(ev.decision), ev.reason,
                           "none", ev.score, f.hr_bpm, f.hr_baseline_dev,
                           f.still_s, f.idle_s, f.hour, flags,
                           (uint32_t)care_now_s());
        syslog(LOG_INFO,
               "[%s] care_event type=%s suppressed=1 reason=%s seq=%u "
               "(no ticket)\n",
               TAG, scene_type, ev.reason, (unsigned)seq);
        return 1;
    }

    care_router_note_action(scene_type);   /* 复用预算/同类冷却/最小间隔记账 */

    /* P2B-2A-FIX（task-72）：ACTION 建 FIRED ticket（TTL=300s），与既有
     * care_route_record 的 ACTION 语义一致；id 记入日志供 App 时间线/排障对账。 */
    ticket_id = care_ticket_create(scene_type, CARE_TICKET_PRIO_NORMAL,
                                   CARE_TICKET_ACTION_TTL_S,
                                   (uint32_t)care_now_s());
    if (ticket_id == CARE_TICKET_ID_INVALID) {
        syslog(LOG_WARNING,
               "[%s] care_event type=%s ticket create failed "
               "(slots full? keep notify)\n", TAG, scene_type);
    }

    seq = care_log_add("care_event", scene_type,
                       care_router_state_name(f.state),
                       care_router_decision_name(ev.decision), ev.reason,
                       "notify", ev.score, f.hr_bpm, f.hr_baseline_dev,
                       f.still_s, f.idle_s, f.hour, flags,
                       (uint32_t)care_now_s());

    post = malloc(sizeof(*post));
    if (post != NULL) {
        post->scene = scene;
        if (lvgl_ui_post(care_scene_post_cb, post) != 0) {
            free(post);
            syslog(LOG_WARNING,
                   "[%s] care_event: lvgl post failed (badge skipped)\n",
                   TAG);
        }
    } else {
        syslog(LOG_WARNING,
               "[%s] care_event: post alloc failed (badge skipped)\n", TAG);
    }

    syslog(LOG_INFO,
           "[%s] care_event type=%s accepted seq=%u ticket=%u suppressed=0\n",
           TAG, scene_type, (unsigned)seq, (unsigned)ticket_id);
    return 0;
}

/* L2 增强：把关怀上下文推给 agent loop（云端→本地兜底整条链路）。
 * 失败静默——L1 气泡已经出去了。
 *
 * PROACTIVE-MVP P0（2026-11-04）：**默认关闭**。
 * 原因：L2 = 云端 LLM -> care 通道 -> 再出声，会和真实语音/音乐抢音频
 * 设备（v142c 刚稳定 TTS，不能引入新的并发路径）；夜间/DND 下也不该再
 * 产生网络+语音副作用。P1 由 App 开关恢复；这里保留实现，编译期打开：
 *   -DPET_CARE_L2_ENABLE=1 或在正式方案里接 App 开关。 */
#ifndef PET_CARE_L2_ENABLE
#define PET_CARE_L2_ENABLE 0
#endif

static void care_start_l2(const char *l1_text)
{
#if !PET_CARE_L2_ENABLE
    /* 只留一条决策日志，便于验收确认"P0 确实没发云端" */
    syslog(LOG_INFO, "[care_router] l2 skipped (P0 default off): %.40s\n",
           (l1_text != NULL) ? l1_text : "");
#else
    agent_msg_t msg = { 0 };
    char prompt[160];

    snprintf(prompt, sizeof(prompt),
             "主动关怀：你刚对用户说了「%s」，请自然地接着这句关心一下用户的近况。"
             "只输出一句不超过30个汉字的关心，不要复述上面这句话，"
             "不要 emoji、列表或行动建议。",
             l1_text);

    strncpy(msg.channel, CARE_CHANNEL, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, CARE_CHAT_ID, sizeof(msg.chat_id) - 1);
    msg.content = strdup(prompt);
    if (msg.content == NULL) {
        return;
    }

    /* render 线程绝不等待队列空间：L1 已保底，队列忙就静默跳过 L2。 */
    if (message_bus_try_push_inbound(&msg) != 0) {
        syslog(LOG_WARNING, "[%s] inbound queue rejected care prompt\n", TAG);
        free(msg.content);
    }
#endif
}

/* ── PROACTIVE-P1A（docs_ble/109 §4/§5.P1a）：Router 结果 -> ticket + log ──
 *
 * ACTION -> 建 FIRED ticket（TTL 5min，覆盖 60s 隐式反馈窗口）+ care_log(action/fire)
 * DEFER  -> 建 DEFERRED ticket（TTL=defer_s，只记录；到点后由既有 1Hz tick 的
 *           自然重评估接管，不新增线程/定时器）+ care_log(defer/defer_ticket)
 * SILENT -> 不建 ticket，只 care_log(silent)（DND/夜间/预算/冷却都能解释）
 *
 * 线程/资源：调用方为 render 线程的 1Hz tick 或 hr_watch 线程；全部走
 * care_ticket/care_log 自己的小 mutex，无阻塞 IO、不写盘。
 *
 * 返回本条 care_log 的 seq（P1b）：调用方在真正开口后用它调
 * care_log_mark_pending(seq, ...)，把 60s 隐式反馈窗口精确绑定到本条。 */
static uint32_t care_route_record(const care_eval_t *ev, uint32_t now_s)
{
    care_features_t f;
    uint8_t flags;
    uint8_t prio;
    uint32_t seq;

    if (ev == NULL) {
        return 0;
    }

    care_router_get_features(&f);
    flags = (uint8_t)((f.dnd ? CARE_LOG_FLAG_DND : 0)
                      | (f.quiet ? CARE_LOG_FLAG_QUIET : 0)
                      | (f.in_call_or_busy ? CARE_LOG_FLAG_BUSY : 0));
    prio = (ev->type != NULL && strcmp(ev->type, "hr_alert") == 0)
               ? CARE_TICKET_PRIO_HIGH : CARE_TICKET_PRIO_NORMAL;

    if (ev->decision == CARE_ACTION) {
        care_ticket_create(ev->type, prio, CARE_TICKET_ACTION_TTL_S, now_s);
        seq = care_log_add("action", ev->type,
                           care_router_state_name(f.state),
                           care_router_decision_name(ev->decision), ev->reason,
                           "fire", ev->score, f.hr_bpm, f.hr_baseline_dev,
                           f.still_s, f.idle_s, f.hour, flags, now_s);
    } else if (ev->decision == CARE_DEFER) {
        care_ticket_defer(ev->type, prio,
                          (uint32_t)((ev->defer_s > 0) ? ev->defer_s : 60),
                          now_s);
        seq = care_log_add("defer", ev->type,
                           care_router_state_name(f.state),
                           care_router_decision_name(ev->decision), ev->reason,
                           "defer_ticket", ev->score, f.hr_bpm,
                           f.hr_baseline_dev, f.still_s, f.idle_s, f.hour,
                           flags, now_s);
    } else {
        seq = care_log_add("silent", ev->type,
                           care_router_state_name(f.state),
                           care_router_decision_name(ev->decision), ev->reason,
                           "none", ev->score, f.hr_bpm, f.hr_baseline_dev,
                           f.still_s, f.idle_s, f.hour, flags, now_s);
    }

    return seq;
}

/* 场景②：空闲关怀。Caller holds s_lock；这里只选模板和更新标量，
 * UI/总线动作由 tick 解锁后执行。 */
static void care_idle_decide(struct care_action_s *action)
{
    time_t now = care_now_s();
    time_t idle_ref = s_last_activity_s > s_last_care_s ? s_last_activity_s
                                                       : s_last_care_s;

    if (s_alarm_stage != ALARM_OFF) {
        return;
    }
    if (now - idle_ref < (time_t)PET_CARE_IDLE_MIN_DEFAULT * 60) {
        return;
    }
    if (s_last_care_s != 0 &&
        now - s_last_care_s < (time_t)PET_CARE_COOLDOWN_MIN_DEFAULT * 60) {
        return;
    }

    /* P0/P1a Router：DND/夜间/预算/同类冷却/走路/刚交互 -> 统一门禁。
     * 夜间不在这里提前 return：让 Router 返回 SILENT(quiet) 以便 care_log
     * 记录（P1a 要求 DND/夜间 SILENT 也可解释）。
     * 60s 内最多问一次：SILENT 时 decide 每 tick 都会被回调，不能刷屏。 */
    if (now - s_last_idle_req_s < 60) {
        return;
    }
    s_last_idle_req_s = now;
    {
        care_eval_t ev = care_router_request("idle");
        /* P1a：ticket + care_log；P1b：记下本条 seq 供反馈窗口绑定 */
        action->care_seq = care_route_record(&ev, (uint32_t)now);
        if (ev.decision != CARE_ACTION) {
            return;             /* SILENT/DEFER 已落 log/ticket，不开口 */
        }
    }

    /* P2A-FIX（看到=听到）：气泡直接用与本地语音相同的 GREET 模板宏。 */
    action->text = care_local_voice_text("idle");
    if (action->text == NULL) {
        return;
    }
    action->emotion = PET_EMOTION_CONFUSED;
    action->start_l2 = true;
    action->type = "idle";
    s_last_care_s = now;
}

/* 场景④：久坐提醒。
 *
 * 判据是真数据 —— pedometer_get_still_seconds()（IMU 带通信号持续低于
 * 0.018g 的时长），不是"用户多久没碰表"（那是 care_idle_decide 的判据）。
 *
 * 三个必要条件缺一不可：
 *   1) 静止时长 ≥ 阈值（默认 45min，care_test 可临时改小）
 *   2) 已"武装"：跨阈值只提醒一次；静止跌破 RESET_SEC 说明人动过了，重新武装
 *   3) 冷却已过（防"动 5 秒又坐下"被连环提醒）
 *
 * Caller holds s_lock；UI/总线动作由 tick 解锁后执行。 */
static void care_sedentary_decide(struct care_action_s *action)
{
    time_t   now = care_now_s();
    uint32_t still_s;

    if (s_sedentary_trigger_s == 0) {
        return;                     /* 关闭 */
    }
    if (s_alarm_stage != ALARM_OFF) {
        return;
    }

    still_s = pedometer_get_still_seconds();

    if (still_s < PET_CARE_SEDENTARY_RESET_SEC) {
        s_sedentary_armed = true;   /* 人动过了，下次静止还可以再提醒 */
        return;
    }
    if (!s_sedentary_armed || still_s < s_sedentary_trigger_s) {
        return;
    }
    if (s_last_sedentary_s != 0 &&
        now - s_last_sedentary_s <
            (time_t)PET_CARE_SEDENTARY_COOLDOWN_MIN * 60) {
        return;
    }

    /* P0 Router：预算/同类冷却/走路(活动)统一门禁；60s 内最多问一次。 */
    if (now - s_last_sed_req_s < 60) {
        return;
    }
    s_last_sed_req_s = now;
    {
        care_eval_t ev = care_router_request("sedentary");
        action->care_seq = care_route_record(&ev, (uint32_t)now);
        if (ev.decision != CARE_ACTION) {
            return;
        }
    }

    /* P2A-FIX（看到=听到）：气泡/语音共用 SEDENTARY 模板宏 */
    action->text = care_local_voice_text("sedentary");
    if (action->text == NULL) {
        return;
    }
    action->emotion = PET_EMOTION_CONFUSED;
    action->start_l2 = true;        /* L2 让云端/端侧把关心说得更自然 */
    action->type = "sedentary";
    s_last_sedentary_s = now;
    s_sedentary_armed  = false;

    snprintf(action->detail, sizeof(action->detail),
             "sedentary fire: still=%us trigger=%us",
             (unsigned)still_s, (unsigned)s_sedentary_trigger_s);
}

/* 场景⑤：步数里程碑庆祝。纯本地、不起 L2、不碰音频设备 ——
 * 所以音乐在播时也可以弹（tick 里对它的门槛只有免打扰）。 */
static void care_step_milestone_decide(struct care_action_s *action)
{
    uint32_t steps = pedometer_get_steps();
    uint32_t target;
    int      idx;
    bool     forced = false;
    int      n = (int)(sizeof(g_step_milestones) /
                       sizeof(g_step_milestones[0]));

    if (s_step_force_target != 0) {
        /* 验收用：数字仍是**真实步数**（care_test 传入当前 steps），
         * 只是不等里程碑到点就提前庆祝 —— 不编造假步数。 */
        target = s_step_force_target;
        s_step_force_target = 0;
        forced = true;
    } else {
        idx = s_step_milestone_idx;
        if (idx < 0 || idx >= n) {
            return;                 /* 里程碑已走完 */
        }
        if (steps < g_step_milestones[idx]) {
            return;
        }
        /* 正常情况一次只跨一个；万一跨了几个（长时间没跑 tick），
         * 只庆祝跨过的最高那个，别连弹好几条。 */
        while (idx + 1 < n && steps >= g_step_milestones[idx + 1]) {
            idx++;
        }
        target = g_step_milestones[idx];
        s_step_milestone_idx = idx + 1;
    }

    snprintf(s_milestone_text, sizeof(s_milestone_text),
             (s_step_milestone_idx & 1)
                 ? "第 %u 步达成，你今天很棒呀"
                 : "已经陪你走了 %u 步啦，继续保持～",
             (unsigned)target);
    action->text = s_milestone_text;
    action->emotion = PET_EMOTION_HAPPY;
    action->start_l2 = false;

    snprintf(action->detail, sizeof(action->detail),
             "step milestone fire: %u steps%s", (unsigned)target,
             forced ? " (care_test forced)" : "");
}

/* 场景①：闹钟状态机。Caller holds s_lock；外部动作延迟到解锁后。 */
static void care_alarm_decide(struct care_action_s *action)
{
    time_t now = care_now_s();

    switch (s_alarm_stage) {
    case ALARM_SET:
        if (now + 60 >= s_alarm_at) {
            action->text = care_pick(g_alarm_pre_templates, 1);
            action->emotion = PET_EMOTION_SPEAKING;
            s_last_care_s = now;
            s_alarm_stage = ALARM_PRE;
        }
        break;

    case ALARM_PRE:
        if (now >= s_alarm_at) {
            /* P2A-FIX（看到=听到）：气泡/语音共用 ALARM 模板宏 */
            action->text = care_local_voice_text("alarm");
            if (action->text == NULL) {
                break;
            }
            action->emotion = PET_EMOTION_HAPPY;
            /* P2A：标记类型供本地语音映射（alarm->ALARM 缓存）；
             * 注意 alarm 不归 care_router 预算/冷却管（tick 里按类型区分）；
             * 本地语音允许穿透 DND/夜间（录音/音乐/busy 仍拦截）。 */
            action->type = "alarm";
            s_last_care_s = now;
            s_alarm_fire_s = now;
            s_alarm_stage = ALARM_FIRE;
        }
        break;

    case ALARM_FIRE:
    case ALARM_ESC:
        if (s_alarm_stage == ALARM_FIRE &&
            now - s_alarm_fire_s >= PET_CARE_ALARM_ESC_SEC &&
            s_last_activity_s < s_alarm_fire_s) {
            action->text = care_pick(g_alarm_esc_templates, 2);
            action->emotion = PET_EMOTION_CONFUSED;
            s_last_care_s = now;
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

/* ── 手机 demo 动作 worker（2026-11-03）─────────────────────────
 * BLE RX 回调线程里直接 care_fire/audio 播放实测会整机 freeze（BT 栈线程
 * 上下文 + 音频/UI 锁）。这里把动作投递到 ai_agent 主进程预创建的 worker，
 * 与 NSH ui demo / App 语音路径的执行上下文一致。 */
#define PET_CARE_DEMO_QDEPTH 8
#define PET_CARE_DEMO_STACK  (24 * 1024)

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int actions[PET_CARE_DEMO_QDEPTH];
    int seconds[PET_CARE_DEMO_QDEPTH];
    int head;
    int count;
    pthread_t thread;
    bool started;
} s_demo_q = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static void *pet_care_demo_worker(void *arg)
{
    (void)arg;
    for (;;) {
        int action, seconds;
        pthread_mutex_lock(&s_demo_q.lock);
        while (s_demo_q.count == 0) {
            pthread_cond_wait(&s_demo_q.cond, &s_demo_q.lock);
        }
        action = s_demo_q.actions[s_demo_q.head];
        seconds = s_demo_q.seconds[s_demo_q.head];
        s_demo_q.head = (s_demo_q.head + 1) % PET_CARE_DEMO_QDEPTH;
        s_demo_q.count--;
        pthread_mutex_unlock(&s_demo_q.lock);

        syslog(LOG_INFO, "[%s] demo worker: action=%d seconds=%d\n", TAG, action, seconds);
        switch (action) {
        case PET_CARE_DEMO_HR:        pet_care_demo_hr_alert(155); break;
        case PET_CARE_DEMO_GREET:     pet_care_demo_greet(); break;
        case PET_CARE_DEMO_WATER:     pet_care_demo_water_reminder(); break;
        case PET_CARE_DEMO_SLEEP:     pet_care_demo_sleep_reminder(); break;
        case PET_CARE_DEMO_WEATHER:   pet_care_demo_weather_change(); break;
        case PET_CARE_DEMO_SEDENTARY: pet_care_demo_sedentary(); break;
        case PET_CARE_DEMO_MILESTONE: pet_care_demo_milestone(); break;
        case PET_CARE_DEMO_ALARM:     (void)pet_care_demo_alarm_after(seconds); break;
        default: break;
        }
    }
    return NULL;
}

static void pet_care_demo_worker_start(void)
{
    pthread_attr_t attr;
    pthread_mutex_lock(&s_demo_q.lock);
    if (s_demo_q.started) { pthread_mutex_unlock(&s_demo_q.lock); return; }
    s_demo_q.started = true;
    pthread_mutex_unlock(&s_demo_q.lock);
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, PET_CARE_DEMO_STACK);
    if (pthread_create(&s_demo_q.thread, &attr, pet_care_demo_worker, NULL) != 0) {
        pthread_mutex_lock(&s_demo_q.lock);
        s_demo_q.started = false;
        pthread_mutex_unlock(&s_demo_q.lock);
        syslog(LOG_ERR, "[%s] demo worker create failed\n", TAG);
    } else {
        syslog(LOG_INFO, "[%s] demo worker started (stack=%d)\n", TAG, PET_CARE_DEMO_STACK);
    }
    pthread_attr_destroy(&attr);
}

typedef struct {
    int action;
    int seconds;
} pet_demo_post_t;

/* 2026-11-03 紧急修复 2：BLE RX 线程/自建 worker 里跑 care 都会 freeze；
 * 真正的主动关怀本来就在 LVGL render 线程（pet_care_tick）里跑，这里改回
 * 同一上下文：BLE handler 只 lvgl_ui_post，回调在 render 线程执行 demo 动作。 */
static void pet_demo_post_cb(void *arg)
{
    pet_demo_post_t *p = (pet_demo_post_t *)arg;
    if (p == NULL) {
        return;
    }
    syslog(LOG_INFO, "[%s] demo post: action=%d seconds=%d\n", TAG, p->action, p->seconds);
    switch (p->action) {
    case PET_CARE_DEMO_HR:        pet_care_demo_hr_alert(155); break;
    case PET_CARE_DEMO_GREET:     pet_care_demo_greet(); break;
    case PET_CARE_DEMO_WATER:     pet_care_demo_water_reminder(); break;
    case PET_CARE_DEMO_SLEEP:     pet_care_demo_sleep_reminder(); break;
    case PET_CARE_DEMO_WEATHER:   pet_care_demo_weather_change(); break;
    case PET_CARE_DEMO_SEDENTARY: pet_care_demo_sedentary(); break;
    case PET_CARE_DEMO_MILESTONE: pet_care_demo_milestone(); break;
    case PET_CARE_DEMO_ALARM:     (void)pet_care_demo_alarm_after(p->seconds); break;
    default: break;
    }
    free(p);
}

void pet_care_demo_async(int action, int seconds)
{
    pet_demo_post_t *p;
    pet_care_init();
    p = malloc(sizeof(*p));
    if (p == NULL) {
        syslog(LOG_ERR, "[%s] demo post OOM\n", TAG);
        return;
    }
    p->action = action;
    p->seconds = seconds;
    if (lvgl_ui_post(pet_demo_post_cb, p) != 0) {
        syslog(LOG_ERR, "[%s] demo post queue failed\n", TAG);
        free(p);
        return;
    }
    syslog(LOG_INFO, "[%s] demo async queued action=%d seconds=%d\n", TAG, action, seconds);
}

int pet_care_init(void)
{
    pthread_mutex_lock(&s_lock);
    if (s_inited) {
        pthread_mutex_unlock(&s_lock);
        return 0;
    }

    s_last_activity_s = care_now_s();   /* 开机视为一次活动，从满 idle 窗起算 */
    s_last_care_s = 0;
    s_last_1hz_s = 0;
    s_alarm_stage = ALARM_OFF;
    s_alarm_minutes = 0;
    s_last_hr_alert_s = 0;
    s_last_sedentary_s = 0;
    s_sedentary_armed = true;
    s_sedentary_trigger_s = PET_CARE_SEDENTARY_MIN_DEFAULT * 60u;
    s_step_milestone_idx = 0;
    s_step_force_target = 0;
    s_rand_state = (unsigned int)time(NULL) ^ (unsigned int)s_last_activity_s;

    s_inited = true;
    pthread_mutex_unlock(&s_lock);

    /* P0：把初始环境同步给 Router（免打扰沿用当前值、久坐阈值用 pet_care 的
     * 默认值；之后 care_test/settings 改动会通过对应 setter 再同步）。 */
    care_router_set_dnd(s_dnd);
    care_router_set_sedentary_threshold_s(s_sedentary_trigger_s);

    syslog(LOG_INFO, "[%s] init ok (idle=%dmin cooldown=%dmin quiet=%d-%d)\n",
           TAG, PET_CARE_IDLE_MIN_DEFAULT, PET_CARE_COOLDOWN_MIN_DEFAULT,
           PET_CARE_QUIET_START_DEFAULT, PET_CARE_QUIET_END_DEFAULT);
    return 0;
}

void pet_care_set_dnd(bool on)
{
    s_dnd = on;
    /* P0：DND 是 Router 的强制 SILENT 输入；夜间由 Router 自己按 hour 判断。 */
    care_router_set_dnd(on);
}

bool pet_care_get_dnd(void)
{
    return s_dnd;
}

void pet_care_tick(void)
{
    struct care_action_s action = { 0 };
    bool run_1hz = false;
    bool walked = false;

    pthread_mutex_lock(&s_lock);
    /* 墙钟秒门控：render 每圈实际耗时随 LVGL 负载变化（实测 ~25ms），
     * 数 tick 会让 1Hz 逻辑退化成几秒一次。 */
    if (s_inited && care_now_s() != s_last_1hz_s) {
        s_last_1hz_s = care_now_s();
        run_1hz = true;

        /* PROACTIVE-MVP P0：把环境/占用同步给 Router，并驱动其 1Hz 采样
         * （心率快照/步数/静止/idle/状态 + 周期决策日志）。
         * Router 只读传感器和自身 RAM 状态：不写盘、不发网络、不起线程。 */
        care_router_set_dnd(s_dnd);
        care_router_set_busy(pcm_file_is_playing());
        care_router_tick();

        /* 走路也算活动：步数涨了就复位 idle（此前只有触摸/按键会复位），
         * 否则"走了一路"仍会被当成空闲/久坐。 */
        {
            uint32_t steps_now = pedometer_get_steps();
            if (steps_now >= s_prev_steps) {
                s_step_accum += steps_now - s_prev_steps;
            } else {
                s_step_accum = 0;   /* pedometer 重启/清零 */
            }
            s_prev_steps = steps_now;
            /* 单步噪声（抬手/走动 1 步）不足以判定“走路”，否则 idle 永不复位；
             * 累计 ≥20 步才认为活动，和 Router 的 steps_delta_60s 口径一致。 */
            if (s_step_accum >= 60) {
                s_step_accum = 0;
                walked = true;
            }
        }

        /* P1a 隐式反馈：PTT/录音也是"用户在响应"（触摸/按键会走
         * pet_care_note_activity，这里补语音按键路径）。 */
        if (voice_channel_is_recording()) {
            care_log_note_activity((uint32_t)care_now_s());
        }

        /* 闹钟先判断：免打扰不吞用户自己设的闹钟 */
        care_alarm_decide(&action);

        /* 主动关怀在两种情况下完全不产生：
         *   1) 免打扰开着
         *   2) 音乐正在播放
         *
         * 为什么音乐也要挡：L2 会走 LLM → TTS，而 TTS 是用 24000Hz 打开
         * 同一个 /dev/audio/pcm0p 的。NuttX 的音频上半部是**设备级共享**的，
         * 第二个客户端一 prepare 就会把正在播的流打断 —— 实测现象就是
         * "30 秒里只有一小段卡住的声音"，日志里能看到播放中途冒出
         *   [pcm_media] prepared: /dev/audio/pcm0p, in=24000Hz out=16000Hz
         *   [pcm_media] stopped (written=0 dropped=0)
         * 也就是 TTS 开了设备、啥也没写、又关掉，而我们的音乐已经断了。
         *
         * 是"不产生"而不是"产生后不显示" —— 后者会白跑一次 L2 的 LLM 调用
         * （断网时那条路径曾经把整个 ai_agent 打死过）。 */
        /* P1a：DND/夜间/音乐占用不再在 tick 层直接跳过，交给 Router 判定
         * （返回 SILENT(*)/DEFER(busy) 后由 care_route_record 落 log/ticket）。
         * 不开口的行为与 P0 一致；这样 care_test 能看到"为什么没开口"。 */
        if (action.text == NULL) {
            care_idle_decide(&action);
        }

        /* 场景④：久坐提醒（真 IMU 静止时长驱动）。与空闲关怀共用
         * "一个 tick 最多产生一条"的约束：上面挑中了就不再调它，
         * 避免同一秒弹两个气泡。门槛同空闲关怀（免打扰 / 音乐在播）。 */
        if (action.text == NULL) {
            care_sedentary_decide(&action);
        }

        /* 场景⑤：步数里程碑庆祝。纯本地气泡、不碰音频设备，所以音乐
         * 在播时也可以弹；只需尊重免打扰。 */
        if (action.text == NULL && !s_dnd) {
            care_step_milestone_decide(&action);
        }
    }
    pthread_mutex_unlock(&s_lock);

    /* 走路 = 活动（不在状态锁内调，避免和 note_activity 的 s_lock 死锁）。 */
    if (walked) {
        pet_care_note_activity();
    }

    if (action.text != NULL) {
        /* 触发依据单独留痕：验收时能回答"它为什么开口"。
         * 气泡文本本身由 care_fire 那行打印。 */
        if (action.detail[0] != '\0') {
            syslog(LOG_INFO, "[%s] %s\n", TAG, action.detail);
        }
        care_fire(action.text, action.emotion, false);
        /* P0 Router 记账：真正开口后才扣预算/记同类冷却（SILENT 不记账）。 */
        if (action.type != NULL) {
            if (care_router_managed_type(action.type)) {
                care_router_note_action(action.type);
                /* P1a 隐式反馈 + P1b 精确绑定：用本条 care_log 的 seq 开
                 * 60s 窗口（避免 hr_watch 线程并发时绑错）。 */
                care_log_mark_pending(action.care_seq,
                                      (uint32_t)care_now_s(), 60);
            }
            /* P2A：主动关怀本地语音（alarm 也在内；仅缓存命中才出声，
             * DND/夜间/录音/忙门禁在 helper 内，失败只留气泡）。 */
            care_local_voice_try(action.type);
        }
        if (action.start_l2) {
            care_start_l2(action.text);
        }
    }

    /* ★ 2026-10-22 起不再调用 hr_monitor_tick()。
     *
     * 原因：hr_monitor.c 是一个**模拟**心率模块（文件第一行就写着
     * "Simulated heart-rate random walk"），它用 rand_r() 随机游走、
     * 并周期性地伪造一段"心率飙升"事件，然后调用 pet_care_hr_report()
     * 在屏幕上弹出真实的心率告警。
     *
     * 也就是说：**手表会随机说"你心率有点高"，而那个数字是随机数。**
     * 这违反项目原则「够用 + 诚实」，而且在演示场合是对评委的误导。
     *
     * 真正的心率告警要等"后台自动测量"做完（需要调度器 + 静默测量 +
     * 有效性判据），那时候把 pet_care_hr_report() 接到真实的测量结果上。
     * 在那之前——**不报比报假的强**。用户现在仍然可以主动去心率页测，
     * 那条路用的是真实传感器数据。 */
#if 0
    if (run_1hz) {
        hr_monitor_tick();
    }
#else
    (void)run_1hz;   /* 摘掉 hr_monitor 后它没有别的消费者了 */
#endif
}

void pet_care_note_activity(void)
{
    bool alarm_acked = false;
    char buf[32] = { 0 };

    pet_care_init();
    pthread_mutex_lock(&s_lock);
    s_last_activity_s = care_now_s();

    /* Stage1 预告以后，任何操作都视为用户已看到并确认叫醒。 */
    if (s_alarm_stage == ALARM_PRE ||
        s_alarm_stage == ALARM_FIRE ||
        s_alarm_stage == ALARM_ESC) {
        time_t now = time(NULL);
        struct tm tm_buf;
        if (gmtime_r(&now, &tm_buf) != NULL) {
            snprintf(buf, sizeof(buf), "[唤醒确认] %02d:%02d",
                     tm_buf.tm_hour, tm_buf.tm_min);
        }
        s_alarm_stage = ALARM_OFF;
        s_alarm_minutes = 0;
        alarm_acked = true;
    }
    pthread_mutex_unlock(&s_lock);

    /* P0 Router：触摸/按键/走路都复位 idle 计时（Router 自己算 idle_s）。 */
    care_router_note_interaction();

    /* P1a 隐式反馈：主动气泡后 60s 内触摸/按键/走路 -> ACCEPTED。 */
    care_log_note_activity((uint32_t)care_now_s());

    /* 不在状态锁内排 LVGL 消息，避免 UI 分配/队列操作扩大临界区。 */
    if (alarm_acked) {
        if (buf[0] != '\0') {
            lvgl_ui_channel_log(buf, true);
        }
        syslog(LOG_INFO, "[%s] alarm acked by activity\n", TAG);
    }
}

int pet_care_alarm_set(int minutes)
{
    if (minutes <= 0 || minutes > 24 * 60) {
        return -1;
    }

    pet_care_init();
    pthread_mutex_lock(&s_lock);
    s_alarm_minutes = minutes;
    s_alarm_at = care_now_s() + (time_t)minutes * 60;
    s_alarm_stage = ALARM_SET;
    pthread_mutex_unlock(&s_lock);

    syslog(LOG_INFO, "[%s] alarm set %dmin\n", TAG, minutes);
    return 0;
}

void pet_care_alarm_cancel(void)
{
    int old_stage;
    int old_minutes;

    pet_care_init();
    pthread_mutex_lock(&s_lock);
    old_stage = (int)s_alarm_stage;
    old_minutes = s_alarm_minutes;
    s_alarm_stage = ALARM_OFF;
    s_alarm_minutes = 0;
    pthread_mutex_unlock(&s_lock);

    if (old_stage != ALARM_OFF) {
        syslog(LOG_INFO, "[%s] alarm cancelled (was stage %d, %dmin)\n",
               TAG, old_stage, old_minutes);
    }
}

void pet_care_hr_report(int bpm)
{
    care_eval_t ev;
    uint32_t care_seq;
    time_t now;

    if (bpm <= 0) {
        return;                 /* 诚实性红线：没测到绝不告警 */
    }

    pet_care_init();

    /* PROACTIVE-MVP P0（2026-11-04）：高心率先过 Router 再决定出不出声。
     *
     * 规则（docs_ble/107）：
     *   - 夜间(22:00-07:00) / 免打扰 / 测量不可信 -> SILENT，只记录；
     *   - 第一次静默高心率 -> SILENT(reason=hr_once)，只记录；
     *   - 连续第 2 次静默高心率（30min 窗口内）才 ACTION 真正告警。
     *
     * 老代码的 60s 冷却由 Router 的"同类冷却 10min + 每 boot 预算 8"取代；
     * 单次高值不再吓用户（可能是运动/佩戴噪声）。 */
    care_router_note_hr_report(bpm);
    ev = care_router_request("hr_alert");
    /* P1a：ticket + care_log；P1b：保存 seq 供 60s 反馈窗口绑定 */
    care_seq = care_route_record(&ev, (uint32_t)care_now_s());
    if (ev.decision != CARE_ACTION) {
        syslog(LOG_INFO,
               "[%s] hr_report silent: bpm=%d decision=%d reason=%s\n",
               TAG, bpm, (int)ev.decision, ev.reason != NULL ? ev.reason : "?");
        return;
    }

    now = care_now_s();
    {
        const char *text;
        char buf[48];

        pthread_mutex_lock(&s_lock);
        s_last_hr_alert_s = now;   /* 兼容旧诊断字段；门禁已由 Router 负责 */
        s_last_care_s = now;
        /* P2A-FIX（看到=听到）：气泡/语音共用 HR 模板宏 */
        text = care_local_voice_text("hr_alert");
        pthread_mutex_unlock(&s_lock);

        if (text != NULL) {
            care_fire(text, PET_EMOTION_CONFUSED, false);
            snprintf(buf, sizeof(buf), "[心率告警] %d bpm", bpm);
            lvgl_ui_channel_log(buf, true);
            care_router_note_action("hr_alert");
            /* P1a 隐式反馈 + P1b 精确绑定：60s 内触摸/按键/PTT/步数 ->
             * ACCEPTED，否则 IGNORED */
            care_log_mark_pending(care_seq, (uint32_t)now, 60);
            /* P2A：心率告警的本地缓存语音（仅命中则异步播；DND/夜间/录音/忙
             * 由 helper 拦截，失败保持"只气泡"）。 */
            care_local_voice_try("hr_alert");
            syslog(LOG_INFO, "[%s] hr alert %d bpm (router ACTION)\n",
                   TAG, bpm);
        }
    }
}

/* ── DEMO-FW（2026-11-03）：演示触发钩子 ──────────────────────────
 * 只给 NSH `ui demo ...` / BLE {"cmd":"demo"} 用。与 care_*_decide 路径
 * 不同，这里**不看冷却/免打扰**：演示要的是"按一下立刻出现"。
 * 全部复用现有 care_fire()/care_start_l2()/cron，不新增线程/文件。
 *
 * DEMO-VOICE-A（2026-11-03）：三个演示的固定文案改为**确定性的**
 * demo_voice_cache.h 宏（与预合成 PCM 逐字一致），先播本地缓存（<1s）；
 * 只有缓存未命中（例如 DEMO_VOICE_CACHE=OFF 的构建）才回落 L2 云端接话。 */

void pet_care_demo_greet(void)
{
    const char *text = DEMO_VOICE_TEXT_GREET;   /* 晚间问候 */

    /* DEMO-PHONE：手机触发时无论当前在哪一页，都先切到小云页 */
    /* 2026-11-03: 显式切页在 BLE 触发时导致 freeze，改用气泡表盘自动切页。 */

    pet_care_init();
    pthread_mutex_lock(&s_lock);
    s_last_care_s = care_now_s();   /* 免得刚打完招呼又立刻触发空闲关怀 */
    pthread_mutex_unlock(&s_lock);

    syslog(LOG_INFO, "[%s] demo greet: %s\n", TAG, text);
    care_fire(text, PET_EMOTION_HAPPY, false);

    if (demo_voice_try_play(text) != 0) {
        care_start_l2(text);        /* 未命中缓存才走 L2 云端接话 */
    }
}

void pet_care_demo_hr_alert(int bpm)
{
    const char *text = DEMO_VOICE_TEXT_HR;
    char buf[48];

    if (bpm <= 0) {
        bpm = 155;
    }

    /* DEMO-PHONE：手机触发时无论当前在哪一页，都先切到小云页 */
    /* 2026-11-03: 显式切页在 BLE 触发时导致 freeze，改用气泡表盘自动切页。 */

    pet_care_init();
    pthread_mutex_lock(&s_lock);
    s_last_hr_alert_s = care_now_s();   /* 强制刷新冷却：连续演示也能触发 */
    s_last_care_s = s_last_hr_alert_s;
    pthread_mutex_unlock(&s_lock);

    care_fire(text, PET_EMOTION_CONFUSED, false);
    snprintf(buf, sizeof(buf), "[心率告警] %d bpm", bpm);
    lvgl_ui_channel_log(buf, true);
    syslog(LOG_INFO, "[%s] demo hr alert %d bpm\n", TAG, bpm);

    if (demo_voice_try_play(text) != 0) {
        care_start_l2(text);        /* 未命中缓存才走 L2 云端接话 */
    }
}

int pet_care_demo_alarm_after(int seconds)
{
    cron_job_t *job;
    int rc;

    if (seconds <= 0 || seconds > 3600) {
        return -1;
    }

    job = calloc(1, sizeof(*job));
    if (job == NULL) {
        return -1;
    }

    strncpy(job->name, "demo_alarm", sizeof(job->name) - 1);
    job->kind = CRON_KIND_AT;
    /* cron 用 time(NULL)（本地墙钟）比较 next_run，这里同一时基 +N 秒。 */
    job->at_epoch = (int64_t)time(NULL) + seconds;
    strncpy(job->channel, AGENT_CHAN_VOICE, sizeof(job->channel) - 1);
    strncpy(job->chat_id, "cron", sizeof(job->chat_id) - 1);
    /* DEMO-VOICE-A：与预合成音频文案一致（到点走 voice 通道时命中缓存） */
    strncpy(job->message, DEMO_VOICE_TEXT_ALARM,
            sizeof(job->message) - 1);
    job->delete_after_run = true;       /* 一次性，响完自动删除 */

    rc = cron_add_job(job);
    syslog(LOG_INFO, "[%s] demo alarm +%ds rc=%d id=%s\n",
           TAG, seconds, rc, job->id);
    free(job);

    return (rc == 0) ? 0 : -1;
}

/* ── DEMO-UI / DEMO-PHONE（2026-11-03）：固定文案主动关怀 ─────────
 * 手机 App（BLE demo）与 NSH ui demo 触发：喝水/睡觉/天气变化/久坐。
 * 统一路径：**先切小云页**（lvgl_ui_channel_navigate_pet，异步投递，保证
 * 从任意页触发都能看到气泡）→ care_fire（气泡 + 历史环）→ 本地预合成
 * 音频（<1s）；只有缓存未命中才回落 L2 云端接话。
 * 文案全部来自 demo_voice_cache.h 宏，与预合成 PCM 逐字一致。 */
static void care_demo_fixed(const char *text, pet_emotion_t emotion)
{
    if (text == NULL) {
        return;
    }

    /* DEMO-PHONE：先切页，再投气泡 —— 两个投递同队列 FIFO，"切页"先于
     * "气泡落地"，气泡才不会因为页面还没建好而丢。 */
    /* 2026-11-03: 显式切页在 BLE 触发时导致 freeze，改用气泡表盘自动切页。 */

    pet_care_init();
    pthread_mutex_lock(&s_lock);
    s_last_care_s = care_now_s();   /* 演示后别立刻再触发空闲关怀 */
    pthread_mutex_unlock(&s_lock);

    syslog(LOG_INFO, "[%s] demo care: %s\n", TAG, text);
    care_fire(text, emotion, false);

    if (demo_voice_try_play(text) != 0) {
        care_start_l2(text);        /* 未命中缓存才走 L2 云端接话 */
    }
}

void pet_care_demo_water_reminder(void)
{
    care_demo_fixed(DEMO_VOICE_TEXT_WATER, PET_EMOTION_HAPPY);
}

void pet_care_demo_sleep_reminder(void)
{
    care_demo_fixed(DEMO_VOICE_TEXT_SLEEP, PET_EMOTION_HAPPY);
}

void pet_care_demo_weather_change(void)
{
    care_demo_fixed(DEMO_VOICE_TEXT_WEATHER, PET_EMOTION_CONFUSED);
}

void pet_care_demo_sedentary(void)
{
    care_demo_fixed(DEMO_VOICE_TEXT_SEDENTARY, PET_EMOTION_CONFUSED);
}

/* DEMO-PHONE：手机触发步数里程碑演示。先切小云页，再让下一次 1Hz tick 弹
 * 真实步数里程碑气泡；步数是动态值，没有预合成音频（本就静音）。 */
void pet_care_demo_milestone(void)
{
    /* 2026-11-03: 显式切页在 BLE 触发时导致 freeze，改用气泡表盘自动切页。 */
    (void)pet_care_debug_force_milestone();
}

/* ── DEMO-UI：10 秒闹钟的异步版 ──────────────────────────────────
 * cron_add_job() 会 cron_save_jobs() -> fopen/fwrite(/data NOR)，一两百毫秒，
 * 绝不能跑在 LVGL 渲染线程（5ms 周期）。这里投到一次性 detached 线程。 */
typedef struct
{
    int seconds;
} demo_alarm_req_t;

static void *demo_alarm_thread(void *arg)
{
    demo_alarm_req_t *req = (demo_alarm_req_t *)arg;
    int seconds = (req != NULL) ? req->seconds : 10;

    free(req);
    (void)pet_care_demo_alarm_after(seconds);
    return NULL;
}

void pet_care_demo_alarm_after_async(int seconds)
{
    demo_alarm_req_t *req;
    pthread_attr_t attr;
    pthread_t tid;
    int ret;

    if (seconds <= 0) {
        seconds = 10;
    }

    req = malloc(sizeof(*req));
    if (req == NULL) {
        return;
    }
    req->seconds = seconds;

    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, 8192);
    ret = pthread_create(&tid, &attr, demo_alarm_thread, req);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        syslog(LOG_WARNING, "[%s] demo alarm thread create failed: %d\n",
               TAG, ret);
        free(req);
    }
}

/* ── 场景④久坐提醒：运行时阈值 ───────────────────────────────── */

void pet_care_set_sedentary_trigger(uint32_t trigger_s)
{
    pet_care_init();
    pthread_mutex_lock(&s_lock);
    s_sedentary_trigger_s = trigger_s;
    s_sedentary_armed = true;       /* 改阈值后允许立刻按新阈值判一次 */
    pthread_mutex_unlock(&s_lock);

    /* P0：Router 的久坐门槛跟随 pet_care（care_test trigger 临时改小时也生效）。 */
    care_router_set_sedentary_threshold_s(trigger_s);

    syslog(LOG_INFO, "[%s] sedentary trigger=%us\n", TAG,
           (unsigned)trigger_s);
}

uint32_t pet_care_get_sedentary_trigger(void)
{
    uint32_t v;

    pet_care_init();
    pthread_mutex_lock(&s_lock);
    v = s_sedentary_trigger_s;
    pthread_mutex_unlock(&s_lock);
    return v;
}

/* ── 验收/诊断接口（NSH care_test）─────────────────────────────── */

void pet_care_debug_dump(void)
{
    uint32_t still_s;
    uint32_t steps;
    uint32_t trigger;
    int      hour;
    bool     dnd;
    bool     quiet;

    pet_care_init();
    pthread_mutex_lock(&s_lock);
    trigger = s_sedentary_trigger_s;
    dnd     = s_dnd;
    pthread_mutex_unlock(&s_lock);

    still_s = pedometer_get_still_seconds();
    steps   = pedometer_get_steps();
    hour    = care_local_hour();
    quiet   = care_is_quiet();

    printf("[care_test] still=%us steps=%u sedentary_trigger=%us "
           "cooldown=%dmin dnd=%d quiet=%d hour=%d\n",
           (unsigned)still_s, (unsigned)steps, (unsigned)trigger,
           PET_CARE_SEDENTARY_COOLDOWN_MIN, (int)dnd, (int)quiet, hour);

    /* PROACTIVE-MVP P0：把 Router 的特征/预算/冷却一起打出来，
     * 验收"为什么没开口/为什么开口"不用再猜。 */
    care_router_dump();

    /* PROACTIVE-P1A：ticket 与决策/反馈日志摘要（验收闭环用）。 */
    care_ticket_dump();
    care_log_dump();
}

int pet_care_debug_force_sedentary(void)
{
    struct care_action_s action = { 0 };

    pet_care_init();
    pthread_mutex_lock(&s_lock);
    if (s_dnd) {
        pthread_mutex_unlock(&s_lock);
        return -1;                  /* 免打扰开着：和真实路径一致，不打扰 */
    }
    action.text = care_pick(g_sedentary_templates,
                            (int)(sizeof(g_sedentary_templates) /
                                  sizeof(g_sedentary_templates[0])));
    action.emotion = PET_EMOTION_CONFUSED;
    action.start_l2 = false;        /* 验收只看气泡，不额外拉一次云端 */
    s_last_sedentary_s = care_now_s();
    s_sedentary_armed = false;
    snprintf(action.detail, sizeof(action.detail),
             "sedentary FORCE (debug): still=%us",
             (unsigned)pedometer_get_still_seconds());
    pthread_mutex_unlock(&s_lock);

    syslog(LOG_INFO, "[%s] %s\n", TAG, action.detail);
    care_fire(action.text, action.emotion, false);
    return 0;
}

int pet_care_debug_force_milestone(void)
{
    uint32_t steps = pedometer_get_steps();

    if (steps == 0) {
        return -1;      /* 一步没走就庆祝，等于编数据 —— 不干 */
    }

    pet_care_init();
    pthread_mutex_lock(&s_lock);
    s_step_force_target = steps;   /* 文案里的步数就是真实步数 */
    pthread_mutex_unlock(&s_lock);

    printf("[care_test] milestone forced (real steps=%u, fires next tick)\n",
           (unsigned)steps);
    return 0;
}
