/*
 * local_lm.c - On-device language model fallback for the agent loop.
 *
 * See local_lm.h.  The model itself lives in the team repo
 * (app/ai_agent/ai_lm.cxx + model/), referenced from CMakeLists.txt through
 * the packages/demos linkfile.
 *
 * LOCAL-FAST A（2026-11-04，docs_ble/108 §2.A/§2.B）：
 *   1) 模板/规则快路径 local_lm_fast_reply()：问候、你是谁、我有点累、
 *      心情不好、夸夸我、谢谢、再见、现在几点/今天几号/星期几（RTC）、
 *      讲个笑话/再讲一个 —— 不跑模型，端到端毫秒级；未命中 -1。
 *      明确不做设备控制（避免本地规则瞎执行），含设备动作词或长句直接放行。
 *   2) 预热 local_lm_warmup_start()：一次性 detached 16KB 线程只做
 *      ai_lm_init()（加载模型 + AllocateTensors），让首次本地兜底不再付
 *      初始化时间（A-FIX：8KB 偏紧，ai_lm_init/AllocateTensors 调用链深）。
 *   3) local_lm_reply() 先走快路径，保证 agent_loop 的本地兜底/弱网优先
 *      都能受益。
 *
 * 约束：不联网、不写 /data、不新增常驻线程（只有一次性 warmup 线程）。
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include "local_lm.h"

#ifdef CONFIG_TFLITEMICRO
#  include "ai_lm.h"
#endif

#define TAG "local_lm"

/* 回复缓冲上限（与 ai_lm.h 的 AI_LM_REPLY_MAX 对齐，避免依赖 TFLM 头） */
#define LOCAL_FAST_MAX 384

/* A-FIX（2026-11-04，perf-fw 复核）：warmup 线程栈 8KB -> 16KB。
 * ai_lm_init() 里 tflite::MicroInterpreter 构造 + AllocateTensors 的调用链
 * 比想象深，8KB 在真机上有栈溢出风险；16KB 仍远小于一次性的临时占用。 */
#define LOCAL_LM_WARMUP_STACK (16 * 1024)

/****************************************************************************
 * LOCAL-FAST A：模板/规则快路径
 *
 * 设计原则：
 *   - 只匹配"纯聊天"的短句；命中后毫秒级返回，完全不跑模型；
 *   - **不做设备控制**：开灯/空调/闹钟/播放/提醒等词一律放行给模型/云端，
 *     避免"本地规则瞎执行"；
 *   - 不编造动作/状态：回复里不承诺任何"我帮你打开了…"；
 *   - 只读 RTC（time/gmtime_r，板上 RTC 存本地时间），不写 /data、不联网。
 ****************************************************************************/

/* 设备/日程控制意图 deny-list（A-FIX：由 local_lm_is_control_intent 导出，
 * 供 agent_loop 复用，保证"本地模板/本地模型不接管控制类请求"的判断只有
 * 一份）。含这些词的句子交回模型/云端，本地快路径不接。 */
static const char *const g_control_intent_kw[] = {
    "打开", "关闭", "关掉", "开启", "设置", "定个", "定一个", "订个",
    "闹钟", "提醒", "定时", "倒计时", "计时", "备忘",
    "播放", "暂停", "下一首", "音量", "亮度",
    "空调", "灯", "音乐", "打电话", "发消息", "导航", "拍照",
    "静音", "勿扰", "连蓝牙", "测心率", "测血氧", "心率", "血氧",
    "关机", "开机", "重启", NULL
};

/* A-FIX：控制意图判定（见 local_lm.h）。纯字符串匹配、无副作用。 */
bool local_lm_is_control_intent(const char *text)
{
    int i;

    if (text == NULL) {
        return false;
    }

    for (i = 0; g_control_intent_kw[i] != NULL; i++) {
        if (strstr(text, g_control_intent_kw[i]) != NULL) {
            return true;
        }
    }

    return false;
}

/* 现在几点 / 今天几号 / 星期几 的 mode */
enum {
    FAST_RTC_TIME = 1,
    FAST_RTC_WEEKDAY = 2,
    FAST_RTC_DATE = 3,
};

static bool fast_has(const char *text, const char *kw)
{
    return (text != NULL) && (kw != NULL) && (strstr(text, kw) != NULL);
}

/* 是否"简单纯聊天短句"：长句或带设备/日程控制意图一律不接。 */
static bool fast_is_simple(const char *text)
{
    if (strlen(text) > 48) {
        return false;
    }

    return !local_lm_is_control_intent(text);
}

static int fast_say(char **out, const char *kw, const char *reply)
{
    *out = strdup(reply);
    if (*out == NULL) {
        return -1;
    }

    syslog(LOG_INFO, "[%s] fast path: kw=%s reply=%s\n", TAG, kw, reply);
    return 0;
}

/* 读 RTC 生成 时间/日期/星期 回答。命中返回 0；读不到时钟返回 -1。 */
static int fast_clock_reply(char **out, const char *kw, int mode)
{
    time_t now = time(NULL);
    struct tm tmv;
    char buf[LOCAL_FAST_MAX];
    static const char *const wk[] = {
        "星期日", "星期一", "星期二", "星期三",
        "星期四", "星期五", "星期六"
    };

    if (gmtime_r(&now, &tmv) == NULL) {
        return -1;
    }

    /* 诚实性（A-FIX 加年份上界）：RTC 未对时（1970/早年）或明显越界
     * （>2100，例如寄存器/RTC 读坏）都不要报"现在几点/今天几号" ——
     * 宁可不接（交给模型/云端），也不给用户一个错的时间。 */
    {
        int year = tmv.tm_year + 1900;

        if (year < 2024 || year > 2100) {
            syslog(LOG_INFO,
                   "[%s] fast path: rtc out of range (%d), skip clock\n",
                   TAG, year);
            return -1;
        }
    }

    switch (mode) {
        case FAST_RTC_TIME:
            snprintf(buf, sizeof(buf), "现在是 %d 点 %02d 分。",
                     tmv.tm_hour, tmv.tm_min);
            break;
        case FAST_RTC_WEEKDAY:
            snprintf(buf, sizeof(buf), "今天是%s。",
                     wk[(tmv.tm_wday >= 0 && tmv.tm_wday < 7) ? tmv.tm_wday : 0]);
            break;
        case FAST_RTC_DATE:
            snprintf(buf, sizeof(buf), "今天是 %d 月 %d 日。",
                     tmv.tm_mon + 1, tmv.tm_mday);
            break;
        default:
            return -1;
    }

    return fast_say(out, kw, buf);
}

int local_lm_fast_reply(const char *user_text, char **out)
{
    if (out == NULL) {
        return -1;
    }
    *out = NULL;

    if (user_text == NULL || user_text[0] == '\0') {
        return -1;
    }

    /* 设备/日程类指令不接（A-FIX：统一走导出的 local_lm_is_control_intent，
     * agent_loop 也用同一份 deny-list 阻止本地模型接管控制类请求）：
     * 避免"帮我定个闹钟"被本地模板回一句"你好"。 */
    if (local_lm_is_control_intent(user_text)) {
        syslog(LOG_INFO,
               "[%s] fast path: control intent -> delegated to model/cloud\n",
               TAG);
        return -1;
    }

    /* 长句不接：48 字节以上交给模型/云端（本地模板只服务短句闲聊）。 */
    if (!fast_is_simple(user_text)) {
        return -1;
    }

    /* 1) 时间 / 日期 / 星期：读 RTC（本地时间）。 */
    if (fast_has(user_text, "现在几点") || fast_has(user_text, "几点了")
        || fast_has(user_text, "什么时间")) {
        if (fast_clock_reply(out, "time", FAST_RTC_TIME) == 0) {
            return 0;
        }
    }

    if (fast_has(user_text, "星期几") || fast_has(user_text, "礼拜几")) {
        if (fast_clock_reply(out, "weekday", FAST_RTC_WEEKDAY) == 0) {
            return 0;
        }
    }

    if (fast_has(user_text, "今天几号") || fast_has(user_text, "几月几号")
        || fast_has(user_text, "今天日期") || fast_has(user_text, "今天多少号")) {
        if (fast_clock_reply(out, "date", FAST_RTC_DATE) == 0) {
            return 0;
        }
    }

    /* 2) 笑话：先处理"再讲一个"（此时也含"笑话"二字），否则第一条。 */
    if (fast_has(user_text, "再讲一个") || fast_has(user_text, "再来一个")
        || fast_has(user_text, "再讲个")) {
        return fast_say(out, "joke2",
            "有一天，0 对 8 说：胖就胖吧，还系什么腰带？");
    }

    if (fast_has(user_text, "笑话") || fast_has(user_text, "逗我笑")) {
        return fast_say(out, "joke1",
            "为什么程序员总分不清万圣节和圣诞节？"
            "因为 Oct 31 等于 Dec 25。");
    }

    /* 3) 固定短句（更具体的先匹配）。 */
    if (fast_has(user_text, "你是谁") || fast_has(user_text, "你叫什么")
        || fast_has(user_text, "介绍一下你自己")
        || fast_has(user_text, "自我介绍")) {
        return fast_say(out, "who",
            "我是小云，你的随身 AI 小助手，可以陪聊天、报时间天气、记提醒。");
    }

    if (fast_has(user_text, "我有点累") || fast_has(user_text, "有点累")
        || fast_has(user_text, "好累") || fast_has(user_text, "累了")) {
        return fast_say(out, "tired",
            "累的话就先停下来歇一会儿吧，我陪着你。");
    }

    if (fast_has(user_text, "心情不好") || fast_has(user_text, "不开心")
        || fast_has(user_text, "难过") || fast_has(user_text, "伤心")
        || fast_has(user_text, "郁闷") || fast_has(user_text, "焦虑")) {
        return fast_say(out, "mood", "我在呢，先陪你缓一缓。");
    }

    if (fast_has(user_text, "夸夸我") || fast_has(user_text, "表扬我")
        || fast_has(user_text, "夸我")) {
        return fast_say(out, "praise",
            "你超棒的！能坚持到现在，本来就很了不起呀～");
    }

    if (fast_has(user_text, "谢谢") || fast_has(user_text, "感谢")) {
        return fast_say(out, "thanks", "不客气呀，能帮上你就好～");
    }

    if (fast_has(user_text, "再见") || fast_has(user_text, "拜拜")
        || fast_has(user_text, "bye")) {
        return fast_say(out, "bye", "拜拜～我会在这儿等你回来。");
    }

    if (fast_has(user_text, "你好") || fast_has(user_text, "您好")
        || fast_has(user_text, "嗨") || fast_has(user_text, "hello")) {
        return fast_say(out, "hello",
            "你好呀！我是小云，很高兴见到你～");
    }

    return -1;   /* 未命中：交给端侧模型/云端 */
}

/****************************************************************************
 * LOCAL-FAST A：预热
 ****************************************************************************/

static volatile bool s_warmup_started;

#if defined(CONFIG_TFLITEMICRO)
static void *local_lm_warmup_thread(void *arg)
{
    (void)arg;

    if (ai_lm_init() == 0) {
        syslog(LOG_INFO, "[%s] warmup done (interpreter ready)\n", TAG);
    } else {
        syslog(LOG_WARNING,
               "[%s] warmup failed (ignored; lazy init will retry)\n", TAG);
    }

    return NULL;
}
#endif

void local_lm_warmup_start(void)
{
    if (s_warmup_started) {
        return;
    }
    s_warmup_started = true;

#ifndef CONFIG_TFLITEMICRO
    syslog(LOG_INFO, "[%s] warmup skipped (no TFLM)\n", TAG);
#else
    {
        pthread_attr_t attr;
        pthread_t tid;
        int ret;

        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, LOCAL_LM_WARMUP_STACK);
        ret = pthread_create(&tid, &attr, local_lm_warmup_thread, NULL);
        pthread_attr_destroy(&attr);

        if (ret != 0) {
            /* 失败忽略：local_lm_reply() 首次调用会再按需初始化 */
            s_warmup_started = false;
            syslog(LOG_WARNING,
                   "[%s] warmup thread create failed: %d (ignored)\n",
                   TAG, ret);
            return;
        }

        syslog(LOG_INFO, "[%s] warmup started (16KB detached thread)\n",
               TAG);
    }
#endif
}

/****************************************************************************
 * Public API
 ****************************************************************************/

bool local_lm_available(void)
{
#ifdef CONFIG_TFLITEMICRO
    return true;
#else
    return false;
#endif
}

int local_lm_reply(const char *user_text, char **out)
{
    if (user_text == NULL || out == NULL) {
        return -1;
    }
    *out = NULL;

    /* LOCAL-FAST A：先走模板快路径（毫秒级，不跑模型）。命中即返回 0；
     * 未命中才落到下面的端侧模型推理。agent_loop 的本地兜底与弱网
     * 本地优先都经这里，因此两边都受益。 */
    if (local_lm_fast_reply(user_text, out) == 0) {
        return 0;
    }

#ifdef CONFIG_TFLITEMICRO
    {
        char reply[AI_LM_REPLY_MAX];

        if (ai_lm_init() != 0) {
            syslog(LOG_WARNING, "[%s] init failed\n", TAG);
            return -1;
        }

        ai_lm_cancel_clear();
        reply[0] = '\0';
        if (ai_lm_agent_reply(user_text, reply, sizeof(reply)) != 0 ||
            reply[0] == '\0') {
            syslog(LOG_WARNING, "[%s] no reply for \"%s\"\n", TAG, user_text);
            return -1;
        }

        *out = strdup(reply);
        if (!*out) {
            return -1;
        }

        syslog(LOG_INFO, "[%s] answered locally: %s\n", TAG, *out);
        return 0;
    }
#else
    return -1;
#endif
}
