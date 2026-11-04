/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include <inttypes.h>

#include "core/agent_loop.h"
#include "core/agent_mem.h"
#include "core/agent_trace.h"
#include "core/context_builder.h"
#include "core/message_bus.h"
#include "voice/voice_channel.h"
#include "core/session_mgr.h"
#include "health/mic_selfcheck.h" /* 麦克风自检 (ask 麦克风自检) */
#include "infra/cron_service.h"      /* DEMO-DIALOG：本地提醒直建 AT 任务 */
#ifdef CONFIG_AI_AGENT_LVGL_UI
/* DEMO-DIALOG 传感器秒答：这两个 .c 只在 LVGL_UI 配置下编译
 * （CMakeLists.txt:145/152），所以 include 与调用一起用同一条件包住。 */
#include "health/hr_measure.h"
#include "health/pedometer.h"
#endif
#include "infra/network_manager.h"
#include "infra/vela_tls.h"   /* P0-NET：[lat] 连接/握手分段 + 保守预连接 */
#include "llm/llm_cache.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "llm/local_lm.h"
#include "tools/skill_loader.h"
#include "tools/tool_guard.h"
#include "tools/tool_registry.h"
#include "ui/persona.h"
#ifdef CONFIG_AI_AGENT_LVGL_UI
#include "ui/lvgl_ui_channel.h"
#endif
#include "agent_compat.h"
#include "agent_config.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"

static const char* TAG = "agent";

/* RCA-FIX-A F8：local_lm 开始/结束耗时（只读日志，线程安全；单调时钟）。 */
static int64_t agent_lat_mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ── LOCAL-FAST B（2026-11-04，docs_ble/108 §2.B/§2.C；B-FIX 按 perf-fw 复核收敛）
 * 只在**离线**（network_get_state()!=CONNECTED）时先让端侧模型抢答，
 * 以及启动时预热端侧模型解释器。
 * 依赖 LOCAL-FAST A（task-24/25）在 local_lm.h 提供的：
 *   void local_lm_warmup_start(void);
 *   int  local_lm_reply(const char *text, char **out);  // 内部先走模板快路径
 * 这些声明已经在本文件 :44 的 #include "llm/local_lm.h" 里，不再重复 extern
 *（v142c 复核 P2 的重复声明已删）。
 * 链接期需要 A 的符号；若构建报 undefined reference，说明 A 未合入。
 *
 * 回滚开关（编译期）：
 *   -DLOCAL_FIRST_WARMUP=0  不调 warmup（不影响其它逻辑）
 *   -DLOCAL_FIRST_ENABLE=0  关闭离线本地优先，恢复 v142c 行为 */
#ifndef LOCAL_FIRST_ENABLE
#define LOCAL_FIRST_ENABLE 1
#endif
#ifndef LOCAL_FIRST_WARMUP
#define LOCAL_FIRST_WARMUP 1
#endif

/* LOCAL-FAST C（2026-11-04）：简单消息本地模板快路径优先。
 * 实测问题：离线时 network 状态未及时翻转，`ask 你好` 仍先等云端失败
 * ~16s 才走端侧快路径。修法：第 0 轮云 LLM 之前先无条件试 A 的模板/规则
 * 快路径（毫秒级、不跑模型、不联网）；命中即本地回答并跳过云端。
 * 回滚开关：-DLOCAL_FAST_PREEMPT=0（不影响 B 的离线本地优先/预热）。 */
#ifndef LOCAL_FAST_PREEMPT
#define LOCAL_FAST_PREEMPT 1
#endif

/* VOICE-A-FIX（task-41，perf-fw A2/P0）：LLM 流式首 token 超时（-ETIMEDOUT）
 * 不参与 router failover / 非流式重试，直接走 local fallback。
 * VOICE-F2（task-60）：语音回合进一步短路成 <=30 字短模板（跳过 ~7s TFLM），
 * 只有真正离线才保留 local_lm。
 * 回滚开关：-DVOICE_A_ETIMEDOUT_SHORTCIRCUIT=0（恢复 failover 行为）。 */
#ifndef VOICE_A_ETIMEDOUT_SHORTCIRCUIT
#define VOICE_A_ETIMEDOUT_SHORTCIRCUIT 1
#endif

/* VOICE-F2（task-60，perf-fw F2）：语音回合"首 token 超时/看门狗超时/联网下
 * 云端失败"兜底不再跑端侧模型（local_lm/TFLM 实测 ~7s），直接给 <=30 字短
 * 模板走既有 outbound dispatch（落屏 + 语音播报）；**真正离线**（uplink_down
 * 或 network 非 CONNECTED）仍保留 local_lm 兜底（离线是它唯一正确的场景）。
 * 回滚开关：-DVOICE_F2_TIMEOUT_SKIP_TFLM=0（恢复原 7s 端侧模型兜底）。 */
#ifndef VOICE_F2_TIMEOUT_SKIP_TFLM
#define VOICE_F2_TIMEOUT_SKIP_TFLM 1
#endif

/* <=30 字、语音友好；不承诺执行设备动作（超时回合工具链可能未完成）。 */
#define VOICE_F2_TIMEOUT_SHORT_MSG "网络有点慢，请再说一次好吗"

/* ── P0-NET（task-48，2026-11-04）：网络/LLM 低延迟收口 ──
 *  P0_NET_PRECONNECT：网络就绪时在 agent 线程里做一次保守 TLS 预连接
 *    （只 TCP+TLS 握手放回池，不发请求；不新增线程）。回滚 -DP0_NET_PRECONNECT=0。
 *  P0_NET_LAT：每轮 [lat] 聚合日志（conn/handshake/ttft/gen/tts_first）。
 *    回滚 -DP0_NET_LAT=0（vela_tls/llm_proxy 的 [lat] 打点保留）。
 *  VOICE_BREVITY_PROMPT：语音 prompt 约束 1–3 句、≤80 字。
 *    回滚 -DVOICE_BREVITY_PROMPT=0（恢复 v114b5 的 90 字文案）。 */
#ifndef P0_NET_PRECONNECT
#define P0_NET_PRECONNECT 1
#endif
#ifndef P0_NET_LAT
#define P0_NET_LAT 1
#endif
#ifndef VOICE_BREVITY_PROMPT
#define VOICE_BREVITY_PROMPT 1
#endif

/* 由 llm_proxy.c 实现：保守预连接当前 LLM 主机（失败静默）。 */
extern int llm_proxy_preconnect(void);

/* 设备控制意图判定 local_lm_is_control_intent() 已由 A-FIX 写进
 * llm/local_lm.h（本文件 :44 已 include），这里不再重复 extern
 * （perf-fw 复核 P2；v142c 的 warmup 重复 extern 也已删）。
 * 用途：开灯/空调/闹钟/播放/测量这类控制消息**绝不能**交给端侧模型的
 * ai_lm 虚拟设备执行器（它会回“好的，已为您打开…”但实际什么都没控制）。 */

#define TOOL_OUTPUT_SIZE (8 * 1024)
#define TOOL_OUTPUT_SIZE_LARGE (16 * 1024)
#define TOOL_OUTPUT_SIZE_MIN (2 * 1024)

/* ── DEMO-FW：演示极速回复（2026-11-03）─────────────────────────
 * 固定短语在 agent_loop 里直接回本地文案，跳过云端 LLM（省 5~20 s）。
 * 默认开（演示构建）；正式构建在编译参数里加 -DDEMO_FAST_REPLY=0 关闭，
 * 或把 handle_demo_fast_path() 连同调用点一起删掉即可完全回滚。
 * 走的是既有本地快通道同一条出站路径：out.from_local=true、仍然经过
 * message_bus -> outbound dispatch，所以手表气泡/历史/App 回包/TTS
 * 与正常回答完全一致（voice 通道照常播报）。 */
#ifndef DEMO_FAST_REPLY
#define DEMO_FAST_REPLY 1
#endif

/* ── Forward declarations ──────────────────────────────────── */

static char* handle_slash_note(const agent_msg_t* msg);
static char* handle_slash_remind(const agent_msg_t* msg);
static bool llm_call_timed_out(uint32_t latency_ms);

/* ── Timeout message constants ─────────────────────────────── */

#define LLM_TIMEOUT_MSG \
    "请求超时，LLM 响应时间过长。请稍后重试，或尝试简化你的问题。"
#define LLM_TIMEOUT_TASK_COMPLETE_MSG \
    "任务已完成，但生成确认消息超时。"

/* VOICE-F2：语音回合超时兜底 -> 短模板（跳过端侧模型）。
 * 返回 malloc 文本；NULL 表示非 voice 通道或开关关闭 -> 调用方走原逻辑。 */
static char* voice_f2_timeout_short_text(const agent_msg_t* msg,
    const char* why)
{
#if VOICE_F2_TIMEOUT_SKIP_TFLM
    size_t n;

    if (msg == NULL
        || strcmp(msg->channel, AGENT_CHAN_VOICE) != 0) {
        return NULL;
    }
    n = strlen(VOICE_F2_TIMEOUT_SHORT_MSG);
    syslog(LOG_WARNING,
        "[%s] voice-f2: %s -> skip local_lm (TFLM ~7s), short template "
        "(%zu chars <=30, dispatch + TTS)\n", TAG, why, n);
    return strdup(VOICE_F2_TIMEOUT_SHORT_MSG);
#else
    (void)msg;
    (void)why;
    return NULL;
#endif
}

/* ── Clock-safe elapsed time calculation ───────────────────── */

static inline uint32_t calc_elapsed_ms(const struct timeval* t0,
    const struct timeval* t1)
{
    int32_t sec_diff = (int32_t)(t1->tv_sec - t0->tv_sec);
    int32_t usec_diff = (int32_t)(t1->tv_usec - t0->tv_usec);

    /* Clock went backwards (NTP jump, manual adjustment) */
    if (sec_diff < 0) {
        syslog(LOG_WARNING, "[%s] Clock went backwards, ignoring\n", TAG);
        return 0;
    }

    /* Microsecond borrow */
    if (usec_diff < 0) {
        sec_diff--;
        usec_diff += 1000000;
    }

    if (sec_diff < 0) {
        return 0;
    }

    return (uint32_t)sec_diff * 1000 + (uint32_t)usec_diff / 1000;
}

/* ── Memory pool (pre-allocated tool output buffers) ───────── */

static agent_mem_pool_t s_tool_pool;
static bool s_pool_ready = false;

/* Build OpenAI-format assistant message with tool_calls at the top level */
static void add_assistant_message(cJSON* messages, const llm_response_t* resp)
{
    cJSON* asst_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(asst_msg, "role", "assistant");

    if (resp->text && resp->text_len > 0) {
        cJSON_AddStringToObject(asst_msg, "content", resp->text);
    } else {
        cJSON_AddNullToObject(asst_msg, "content");
    }

    /* Kimi thinking mode: echo back reasoning_content or the API returns 400 */
    if (resp->reasoning_content && resp->reasoning_content[0]) {
        cJSON_AddStringToObject(asst_msg, "reasoning_content",
            resp->reasoning_content);
    }

    if (resp->call_count > 0) {
        cJSON* tool_calls = cJSON_CreateArray();
        for (int i = 0; i < resp->call_count; i++) {
            const llm_tool_call_t* call = &resp->calls[i];
            cJSON* tc = cJSON_CreateObject();
            cJSON_AddStringToObject(tc, "id", call->id);
            cJSON_AddStringToObject(tc, "type", "function");

            cJSON* func_obj = cJSON_CreateObject();
            cJSON_AddStringToObject(func_obj, "name", call->name);
            cJSON_AddStringToObject(func_obj, "arguments",
                call->input ? call->input : "{}");
            cJSON_AddItemToObject(tc, "function", func_obj);
            cJSON_AddItemToArray(tool_calls, tc);
        }
        cJSON_AddItemToObject(asst_msg, "tool_calls", tool_calls);
    }

    cJSON_AddItemToArray(messages, asst_msg);
}

/* Auto-inject channel/chat_id into cron_add input JSON. */
static char* inject_cron_context(const char* tool_name,
    const char* input_json, const char* channel, const char* chat_id)
{
    if (strcmp(tool_name, "cron_add") != 0) {
        return NULL;
    }
    if (!channel || !channel[0] || !chat_id || !chat_id[0]) {
        return NULL;
    }

    cJSON* root = cJSON_Parse(input_json);
    if (!root) {
        return NULL;
    }

    cJSON_DeleteItemFromObject(root, "channel");
    cJSON_DeleteItemFromObject(root, "chat_id");
    cJSON_AddStringToObject(root, "channel", channel);
    cJSON_AddStringToObject(root, "chat_id", chat_id);

    char* patched = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return patched;
}

/* Parallel tool execution context */
typedef struct {
    const llm_tool_call_t* call;
    char* output;
    size_t output_size;
    bool from_pool;
    char channel[16];
    char chat_id[64];
} tool_task_t;

static void* tool_exec_thread(void* arg)
{
    tool_task_t* t = (tool_task_t*)arg;

    t->output[0] = '\0';
    char* patched = inject_cron_context(
        t->call->name, t->call->input, t->channel, t->chat_id);
    agent_tool_exec_streamed(t->call->name,
        patched ? patched : t->call->input,
        t->output, t->output_size);
    free(patched);
    syslog(LOG_INFO, "[%s] Tool %s result: %d bytes\n",
        TAG, t->call->name, (int)strlen(t->output));
    return NULL;
}

/* Add one role:"tool" message per tool result (OpenAI format).
 * When call_count > 1, all tools run in parallel threads.
 * Uses memory pool for parallel buffers, with heap fallback. */
static void add_tool_result_messages(cJSON* messages,
    const llm_response_t* resp, char* tool_output,
    size_t tool_output_size, const char* msg_channel,
    const char* msg_chat_id)
{
    int n = resp->call_count;

    if (n == 1) {
        const llm_tool_call_t* call = &resp->calls[0];

        syslog(LOG_INFO, "[%s] Tool call: %s args=%.500s\n", TAG,
            call->name, call->input ? call->input : "(null)");
        tool_output[0] = '\0';
        char* patched = inject_cron_context(
            call->name, call->input, msg_channel, msg_chat_id);
        agent_tool_exec_streamed(call->name,
            patched ? patched : call->input,
            tool_output, tool_output_size);
        free(patched);
        syslog(LOG_INFO, "[%s] Tool %s result: %d bytes\n", TAG,
            call->name, (int)strlen(tool_output));

        cJSON* result_msg = cJSON_CreateObject();
        cJSON_AddStringToObject(result_msg, "role", "tool");
        cJSON_AddStringToObject(result_msg, "tool_call_id", call->id);
        cJSON_AddStringToObject(result_msg, "content", tool_output);
        cJSON_AddItemToArray(messages, result_msg);
        return;
    }

    /* Parallel path */
    tool_task_t tasks[AGENT_MAX_TOOL_CALLS];
    pthread_t threads[AGENT_MAX_TOOL_CALLS];
    size_t par_buf_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE_LARGE, TOOL_OUTPUT_SIZE_MIN);

    for (int i = 0; i < n; i++) {
        tasks[i].call = &resp->calls[i];
        strncpy(tasks[i].channel, msg_channel ? msg_channel : "",
            sizeof(tasks[i].channel) - 1);
        strncpy(tasks[i].chat_id, msg_chat_id ? msg_chat_id : "",
            sizeof(tasks[i].chat_id) - 1);

        char* buf = s_pool_ready
            ? agent_pool_acquire(&s_tool_pool)
            : NULL;
        if (buf) {
            tasks[i].output = buf;
            tasks[i].output_size = s_tool_pool.buf_size;
            tasks[i].from_pool = true;
        } else {
            tasks[i].output = calloc(1, par_buf_size);
            tasks[i].output_size = par_buf_size;
            tasks[i].from_pool = false;
        }

        if (!tasks[i].output) {
            threads[i] = 0;
            continue;
        }

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 16 * 1024);
        if (pthread_create(&threads[i], &attr,
                tool_exec_thread, &tasks[i])
            != 0) {
            syslog(LOG_ERR, "[%s] Failed to spawn thread for tool %s\n",
                TAG, resp->calls[i].name);
            threads[i] = 0;
        }
        pthread_attr_destroy(&attr);
    }

    /* Join and collect results, dedup by tool_call_id */
    char seen_ids[AGENT_MAX_TOOL_CALLS][32];
    int seen_count = 0;

    for (int i = 0; i < n; i++) {
        if (threads[i]) {
            pthread_join(threads[i], NULL);
        }

        bool dup = false;
        for (int j = 0; j < seen_count; j++) {
            if (strcmp(seen_ids[j], resp->calls[i].id) == 0) {
                dup = true;
                break;
            }
        }

        if (dup) {
            syslog(LOG_WARNING,
                "[%s] Skipping duplicate tool_call_id: %s\n",
                TAG, resp->calls[i].id);
        } else {
            strncpy(seen_ids[seen_count++], resp->calls[i].id,
                sizeof(seen_ids[0]) - 1);

            cJSON* result_msg = cJSON_CreateObject();
            cJSON_AddStringToObject(result_msg, "role", "tool");
            cJSON_AddStringToObject(result_msg, "tool_call_id",
                resp->calls[i].id);
            cJSON_AddStringToObject(result_msg, "content",
                (tasks[i].output && tasks[i].output[0])
                    ? tasks[i].output
                    : "{}");
            cJSON_AddItemToArray(messages, result_msg);
        }

        if (tasks[i].from_pool) {
            agent_pool_release(&s_tool_pool, tasks[i].output);
        } else {
            free(tasks[i].output);
        }
    }
}

/* ── Fast path counters ─────────────────────────────────── */

static int s_fast_path_count = 0;
static int s_llm_path_count = 0;

/* ── Natural language fast path (keyword → direct tool call) ── */

static bool contains_any(const char* text, const char** keywords)
{
    for (int i = 0; keywords[i]; i++) {
        if (strcasestr(text, keywords[i])) {
            return true;
        }
    }
    return false;
}

/* Intent table: each entry maps keywords → tool + args + output size */

typedef struct {
    const char** keywords;
    const char* tool_name;
    const char* tool_args;
    size_t output_size;
} nl_intent_t;

/* ★ 2026-10-25 补缺（审计 B）：这些常见说法原来不命中、绕 LLM 明显变慢。
 * 注意 strcasestr 是连续子串匹配，加词要覆盖口语变体。 */
static const char* kw_time[]       = { "几点了", "几点钟", "现在几点", "什么时间",
                                        "what time", "时间是多少", NULL };
static const char* kw_battery[]    = { "电量多少", "电池电量", "还剩多少电",
                                        "还有多少电", "电量是多少", "battery", NULL };
static const char* kw_heartrate[]  = { "心率多少", "heart rate", "心率是多",
                                        "测一下心率", "心率是多少", NULL };
static const char* kw_steps[]      = { "走了多少步", "今天走了多少", "steps", "步数", NULL };
/* 血氧: get_spo2 有历史值直接报、无则起异步测量(20s 后语音播报),
 * 输出本身就是口语中文(不是 JSON), 走快通道正合适。 */
static const char* kw_spo2[]       = { "血氧多少", "血氧是多少", "spo2",
                                        "血氧饱和度", "测一下血氧", NULL };
static const char* kw_micself[]    = { "麦克风自检", "麦克风检测", "麦克风测试",
                                        "话筒自检", "mic check", "mic test",
                                        "microphone test", NULL };
static const char* kw_pause[]      = { "暂停音乐", "暂停播放", "暂停一下",
                                        "pause music", NULL };
static const char* kw_stop[]       = { "停止播放", "停止音乐", "stop music", NULL };
static const char* kw_resume[]     = { "继续播放", "继续放", "接着播",
                                        "继续播", NULL };

static const nl_intent_t s_intents[] = {
    { kw_time,      "get_current_time", "{}",  256  },
    { kw_battery,   "get_battery",      "{}",  512  },
    { kw_heartrate, "get_heartrate",    "{}",  256  },
    { kw_steps,     "get_steps",        "{}",  256  },
#ifdef CONFIG_AI_AGENT_LVGL_UI
    { kw_spo2,      "get_spo2",         "{}",  128  },
#endif
    { kw_pause,     "music_pause",      "{}",  256  },
    { kw_stop,      "music_stop",       "{}",  256  },
    { kw_resume,    "music_resume",     "{}",  256  },
    { NULL,         NULL,               NULL,  0    },
};

/* ═══ v106 S5：情绪词本地关怀（不走云端 LLM，屏幕秒出） ═══
 * 这些是"陪一句"场景，规则回复即可；是否播报仍由 outbound 决定。 */
static const char* kw_mood[] = {
    "心情不好", "心情很差", "不开心", "难过", "伤心", "郁闷",
    "烦躁", "焦虑", "压力大", "好累", "孤独", "委屈", "生气", "emo",
    NULL };

static const char* mood_replies[] = {
    "我在呢，先陪你缓一缓。",
    "听到了，不用一个人扛着。",
    "先深呼吸，我陪着你。",
    NULL };

/* ═══ v106 S3：快通道工具输出人话化 ═══
 * 工具给 LLM 的输出保持结构化/原样，这里只把"直接当回复"的 JSON 转成人话。 */
static char* humanize_tool_reply(const char* tool, char* raw)
{
    cJSON* r;
    char* out;

    if (raw == NULL || tool == NULL) {
        return raw;
    }

    if (strcmp(tool, "get_battery") == 0) {
        int level = -1;
        int charging = 0;
        r = cJSON_Parse(raw);
        if (r != NULL) {
            cJSON* l = cJSON_GetObjectItem(r, "level");
            cJSON* c = cJSON_GetObjectItem(r, "charging");
            if (cJSON_IsNumber(l)) level = (int)l->valuedouble;
            if (cJSON_IsNumber(c)) charging = (int)c->valuedouble;
            cJSON_Delete(r);
        }
        if (level >= 0) {
            out = malloc(96);
            if (out != NULL) {
                snprintf(out, 96, charging ? "电量 %d%%，正在充电。"
                                           : "电量还剩 %d%%。", level);
                free(raw);
                return out;
            }
        }
        return raw;
    }

    if (strcmp(tool, "get_heartrate") == 0) {
        int bpm = -1;
        r = cJSON_Parse(raw);
        if (r != NULL) {
            cJSON* b = cJSON_GetObjectItem(r, "bpm");
            if (cJSON_IsNumber(b)) bpm = (int)b->valuedouble;
            cJSON_Delete(r);
        }
        if (bpm > 0) {
            out = malloc(96);
            if (out != NULL) {
                snprintf(out, 96, "当前心率 %d 次/分。", bpm);
                free(raw);
                return out;
            }
        }
        return raw;
    }

    if (strcmp(tool, "get_steps") == 0) {
        int steps = -1;
        r = cJSON_Parse(raw);
        if (r != NULL) {
            cJSON* s = cJSON_GetObjectItem(r, "steps");
            if (cJSON_IsNumber(s)) steps = (int)s->valuedouble;
            cJSON_Delete(r);
        }
        if (steps >= 0) {
            out = malloc(96);
            if (out != NULL) {
                snprintf(out, 96, "今天走了 %d 步。", steps);
                free(raw);
                return out;
            }
        }
        return raw;
    }

    /* get_current_time 已在工具层人话化；get_spo2 本身就是中文口语 */
    return raw;
}

/* 麦克风自检 (特殊分支, 不进 s_intents 表):
 *
 * 表驱动的快通道只能把工具的原始 JSON 当回复交出去 —— get_heartrate 之类
 * 就是那样 ({...})。麦克风自检要的是一句人能读、能被 TTS 念出来的结论,
 * 所以在这里显式调用: 跑一次自检 -> 中文结论 -> 上屏/播报 -> 同时作为
 * 控制台回复返回。
 *
 * 为什么必须走本地快通道而不是 LLM 工具: 这正是云端 "No available backend"
 * 时最需要跑的诊断; 而且它本来就是设备侧的事, 没有理由绕一圈模型。
 *
 * 采集 3 秒是阻塞的, 但这条线程是 agent loop 自己的 worker, 不碰 LVGL
 * render 线程 (lvgl_ui_channel_send_kind 只投递)。 */
static char* handle_mic_selfcheck_fast_path(const char* text)
{
    mic_selfcheck_result_t r;
    char report[192];
    char* reply;

    if (!contains_any(text, kw_micself)) {
        return NULL;
    }

    mic_selfcheck_run(&r, true);
    mic_selfcheck_report(&r, report, sizeof(report));

    s_fast_path_count++;
    syslog(LOG_INFO,
        "[%s] NL fast path: mic_selfcheck (fast=%d llm=%d) verdict=%s "
        "rms=%.1f peak=%d distinct=%u gain=%d\n",
        TAG, s_fast_path_count, s_llm_path_count,
        r.mic_ok ? "mic OK" : "mic NO-RESPONSE",
        r.rms, (int)r.peak, (unsigned)r.distinct,
        r.gain_known ? r.adc_gain_db : 0);

#ifdef CONFIG_AI_AGENT_LVGL_UI
    /* 结论上屏 + 表情 + (FINAL) 语音播报, 与 hr_measure 的报喜路径一致 */
    {
        extern int lvgl_ui_channel_send_kind(const char* text, uint8_t kind);
        (void)lvgl_ui_channel_send_kind(report, AGENT_MSG_KIND_FINAL);
    }
#endif

    reply = strdup(report);
    return reply;
}

#if DEMO_FAST_REPLY
/* DEMO-FW：演示固定短语 -> 本地文案（不依赖网络/模型）。
 * 只接管 voice / ble / cli 三个通道；其它通道保持原行为。
 * 注：现在几点、天气、电量、心率、步数等本来就由下面的
 * handle_nl_fast_path() 本地工具覆盖（也不走云端），这里不重复。
 *
 * 匹配是"整句"匹配（去掉首尾空白/常见中英标点后逐字相等），不是子串
 * 匹配 —— 否则"你好，帮我定个闹钟"会被误判成打招呼。 */

/* 去掉尾部标点/空白后的有效长度（UTF-8 标点按整串比较，不切半个字）。 */
static size_t demo_trimmed_len(const char* s)
{
    static const char* tails[] =
    {
        "，", "。", "！", "？", "；", "：", "、", "～", "…", "“", "”",
        ",", ".", "!", "?", ";", ":", "~", " ", "\t", "\r", "\n",
        NULL
    };
    size_t n = strlen(s);
    bool trimmed = true;

    while (n > 0 && trimmed)
      {
        int i;

        trimmed = false;
        for (i = 0; tails[i] != NULL; i++)
          {
            size_t pl = strlen(tails[i]);

            if (n >= pl && memcmp(s + n - pl, tails[i], pl) == 0)
              {
                n -= pl;
                trimmed = true;
                break;
              }
          }
      }

    return n;
}

static bool demo_text_is(const char* text, const char* phrase)
{
    size_t plen = strlen(phrase);

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
      {
        text++;
      }

    return demo_trimmed_len(text) == plen && strncmp(text, phrase, plen) == 0;
}


/* ── DEMO-DIALOG（2026-11-03）：动态本地秒答 ───────────────────
 * 静态短语表放不下的、需要读设备状态/写 cron 的秒答集中在这里。
 * 全部仍是"整句"语义（demo_text_is / 严格的 N秒后提醒我 模式），
 * 避免子串误伤正常提问（中文 UTF-8 按整串比较）。
 * 返回堆字符串由调用方持有（与静态表分支一致）；NULL 表示不命中。 */

/* 解析"<N>秒后提醒我[内容]" / "<N>分钟后提醒我[内容]"。
 * 允许首尾空白与句末标点（demo_trimmed_len 已处理）。
 * 返回 true 时写 *delay_s（1..3600 秒）与可选内容 content（可空串）。
 * 任何多余/错位的字都不命中 —— 这是"整句"约束，不是子串搜索。 */
static bool demo_parse_reminder(const char* text, int* delay_s,
    char* content, size_t content_sz)
{
    static const char* k_suffix = "后提醒我";   /* 12 bytes UTF-8 */
    size_t n;
    size_t i = 0;
    long num = 0;
    int digits = 0;
    int unit_s = 1;
    size_t slen = strlen(k_suffix);

    if (content == NULL || content_sz == 0)
      {
        return false;
      }
    content[0] = '\0';

    if (text == NULL)
      {
        return false;
      }

    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n')
      {
        text++;
      }
    n = demo_trimmed_len(text);

    /* 1) 前导数字（最多 4 位） */
    while (i < n && text[i] >= '0' && text[i] <= '9' && digits < 4)
      {
        num = num * 10 + (long)(text[i] - '0');
        i++;
        digits++;
      }
    if (digits == 0)
      {
        return false;
      }

    while (i < n && (text[i] == ' ' || text[i] == '\t'))
      {
        i++;
      }

    /* 2) 单位：分钟/秒钟/秒/分（UTF-8 字节数：分=3, 分钟=6, 秒=3, 秒钟=6） */
    if (i + 6 <= n && memcmp(text + i, "分钟", 6) == 0)
      {
        unit_s = 60;
        i += 6;
      }
    else if (i + 6 <= n && memcmp(text + i, "秒钟", 6) == 0)
      {
        unit_s = 1;
        i += 6;
      }
    else if (i + 3 <= n && memcmp(text + i, "秒", 3) == 0)
      {
        unit_s = 1;
        i += 3;
      }
    else if (i + 3 <= n && memcmp(text + i, "分", 3) == 0)
      {
        unit_s = 60;
        i += 3;
      }
    else
      {
        return false;
      }

    while (i < n && (text[i] == ' ' || text[i] == '\t'))
      {
        i++;
      }

    /* 3) 必须紧跟"后提醒我" */
    if (i + slen > n || memcmp(text + i, k_suffix, slen) != 0)
      {
        return false;
      }
    i += slen;

    /* 4) 剩余是可选提醒内容（如"10秒后提醒我喝水"里的"喝水"）。
     * 允许一个分隔逗号/空格（"10秒后提醒我，喝水"）。 */
    while (i < n && (text[i] == ' ' || text[i] == '\t'))
      {
        i++;
      }
    if (i + 3 <= n && memcmp(text + i, "，", 3) == 0)
      {
        i += 3;
        while (i < n && (text[i] == ' ' || text[i] == '\t'))
          {
            i++;
          }
      }
    if (i < n)
      {
        size_t clen = n - i;

        if (clen >= content_sz)
          {
            clen = content_sz - 1;
          }
        memcpy(content, text + i, clen);
        content[clen] = '\0';
      }

    *delay_s = (int)(num * unit_s);
    return *delay_s >= 1 && *delay_s <= 3600;
}

/* 人格切换关键词：整句匹配。返回回复文案（已 strdup），未命中返回 NULL。 */
static char* demo_persona_reply(const char* channel, const char* text)
{
    persona_id_t id;
    const char* ok_reply;
    const char* action;

    if (demo_text_is(text, "温柔一点") || demo_text_is(text, "温柔点")
        || demo_text_is(text, "切换到温柔模式") || demo_text_is(text, "温柔模式")
        || demo_text_is(text, "恢复温柔") || demo_text_is(text, "恢复温柔模式"))
      {
        id = PERSONA_GENTLE;
        ok_reply = "好呀，我温柔一点～";
        action = "persona_gentle";
      }
    else if (demo_text_is(text, "毒舌一点") || demo_text_is(text, "毒舌点")
        || demo_text_is(text, "恢复毒舌") || demo_text_is(text, "恢复到毒舌")
        || demo_text_is(text, "切换到毒舌模式") || demo_text_is(text, "毒舌模式"))
      {
        id = PERSONA_SASSY;
        ok_reply = "行，那我可就不留情面了～";
        action = "persona_sassy";
      }
    else if (demo_text_is(text, "元气一点") || demo_text_is(text, "元气点")
        || demo_text_is(text, "恢复元气") || demo_text_is(text, "切换到元气模式")
        || demo_text_is(text, "元气模式"))
      {
        id = PERSONA_CHEER;
        ok_reply = "好耶，元气满满！";
        action = "persona_cheer";
      }
    else
      {
        return NULL;
      }

    {
        int ret = persona_set(id);

        s_fast_path_count++;
        syslog(LOG_INFO,
               "[demo-fast] chan=%s action=%s -> persona %s (set=%d, "
               "fast=%d llm=%d)\n",
               channel ? channel : "?", action,
               persona_name(id) ? persona_name(id) : "?", ret,
               s_fast_path_count, s_llm_path_count);
        return strdup(ret == OK ? ok_reply : "人格切换失败，稍后再试一次吧。");
    }
}

#ifdef CONFIG_AI_AGENT_LVGL_UI
/* 传感器秒答：只读设备当前值，不发起测量（测量要 8~30s，破坏"秒答"）。
 * 没测到过就如实说"还没测到"，绝不显示 0（健康数据诚实性红线）。
 * hr_measure.c / pedometer.c 只在 CONFIG_AI_AGENT_LVGL_UI 下编译
 * （CMakeLists.txt:145/152），所以整个函数跟着条件编译。 */
static char* demo_sensor_reply(const char* channel, const char* text)
{
    char buf[96];

    if (demo_text_is(text, "我走了多少步") || demo_text_is(text, "今天走了多少步")
        || demo_text_is(text, "我现在走了多少步")
        || demo_text_is(text, "我走了几步") || demo_text_is(text, "今天走了几步")
        || demo_text_is(text, "走了多少步"))
      {
        uint32_t steps = pedometer_get_steps();
        bool alive = pedometer_is_alive();

        s_fast_path_count++;
        syslog(LOG_INFO,
               "[demo-fast] chan=%s action=steps steps=%u alive=%d "
               "(fast=%d llm=%d)\n",
               channel ? channel : "?", (unsigned)steps, alive,
               s_fast_path_count, s_llm_path_count);
        if (!alive)
          {
            return strdup("计步还没跑起来，走几步再问我一次～");
          }
        /* 不声称"今天"：pedometer_get_steps() 是本次开机累计的实时值，
         * 断电重启会清零，说"今天"会高估（对账口径见 pedometer.h）。 */
        snprintf(buf, sizeof(buf), "你已经走了 %u 步。", (unsigned)steps);
        return strdup(buf);
      }

    if (demo_text_is(text, "现在心率多少") || demo_text_is(text, "我心率多少")
        || demo_text_is(text, "我的心率是多少") || demo_text_is(text, "心率多少")
        || demo_text_is(text, "我现在心率多少")
        || demo_text_is(text, "我心率是多少"))
      {
        int bpm = hr_measure_last_bpm();
        bool ok = hr_measure_last_ok();
        bool running = hr_measure_is_running();

        s_fast_path_count++;
        syslog(LOG_INFO,
               "[demo-fast] chan=%s action=heartrate bpm=%d ok=%d running=%d "
               "(fast=%d llm=%d)\n",
               channel ? channel : "?", bpm, ok, running,
               s_fast_path_count, s_llm_path_count);
        if (running)
          {
            return strdup("正在测心率，稍等一下～");
          }
        if (!ok || bpm <= 0)
          {
            return strdup("还没测到心率，按一下心率页的“测心率”吧。");
          }
        /* 说"最近一次测到"而不是"当前"：last_bpm 可能是上一轮的值，
         * 健康数据不夸大（hr_measure.h 的诚实性约定）。 */
        snprintf(buf, sizeof(buf), "最近一次测到的心率是 %d 次/分。", bpm);
        return strdup(buf);
      }

    return NULL;
}
#endif /* CONFIG_AI_AGENT_LVGL_UI */

/* 提醒类："10秒后提醒我" / "1分钟后提醒我" / "30秒后提醒我喝水"。
 * 直接建一条 CRON_KIND_AT 一次性任务，走与 cron_add 工具相同的
 * cron_service（到点从 outbound 下发 FINAL，voice 通道会播报）。 */
static char* demo_reminder_reply(const char* channel, const char* chat_id,
    const char* text)
{
    char content[80];
    char message[160];
    char buf[96];
    cron_job_t job;
    int delay_s = 0;
    int err;

    if (!demo_parse_reminder(text, &delay_s, content, sizeof(content)))
      {
        return NULL;
      }

    if (content[0] != '\0')
      {
        snprintf(message, sizeof(message), "提醒你：%s", content);
      }
    else
      {
        snprintf(message, sizeof(message), "时间到啦，这是你刚才设的提醒～");
      }

    memset(&job, 0, sizeof(job));
    strncpy(job.name, "demo_remind", sizeof(job.name) - 1);
    strncpy(job.message, message, sizeof(job.message) - 1);
    job.kind = CRON_KIND_AT;
    job.at_epoch = (int64_t)time(NULL) + delay_s;
    job.delete_after_run = true;
    if (channel != NULL)
      {
        strncpy(job.channel, channel, sizeof(job.channel) - 1);
      }
    if (chat_id != NULL)
      {
        strncpy(job.chat_id, chat_id, sizeof(job.chat_id) - 1);
      }

    err = cron_add_job(&job);
    s_fast_path_count++;
    syslog(LOG_INFO,
           "[demo-fast] chan=%s action=remind delay=%ds add=%d msg=\"%s\" "
           "(fast=%d llm=%d)\n",
           channel ? channel : "?", delay_s, err, message,
           s_fast_path_count, s_llm_path_count);

    if (err != OK)
      {
        return strdup("提醒任务太多啦，先删掉几个再试吧。");
      }

    if (delay_s >= 60 && (delay_s % 60) == 0)
      {
        snprintf(buf, sizeof(buf), "好，%d 分钟后提醒你。", delay_s / 60);
      }
    else
      {
        snprintf(buf, sizeof(buf), "好，%d 秒后提醒你。", delay_s);
      }
    return strdup(buf);
}

/* 动态秒答总入口：人格 / 传感器 / 提醒。未命中返回 NULL，
 * 交回静态短语表（最后才轮到 handle_nl_fast_path）。 */
static char* demo_dynamic_reply(const char* channel, const char* chat_id,
    const char* text)
{
    char* reply;

    reply = demo_persona_reply(channel, text);
    if (reply != NULL)
      {
        return reply;
      }

#ifdef CONFIG_AI_AGENT_LVGL_UI
    reply = demo_sensor_reply(channel, text);
    if (reply != NULL)
      {
        return reply;
      }
#endif

    return demo_reminder_reply(channel, chat_id, text);
}

static char* handle_demo_fast_path(const char* channel, const char* chat_id,
    const char* text)
{
    static const struct
    {
        const char* keywords[6];
        const char* reply;
    } demo_replies[] =
    {
        { { "你好", "您好", "你好呀", "你好啊", "嗨", NULL },
          "你好呀！我是小云，很高兴见到你～" },
        { { "你是谁", "你是哪位", "你叫什么", "你叫什么名字",
            "介绍一下你自己", "自我介绍" },
          "我是小云，你的随身 AI 小助手，可以陪聊天、报时间天气、记提醒。" },
        { { "讲个笑话", "说个笑话", "来个笑话", "讲笑话", "笑话", NULL },
          "为什么程序员总分不清万圣节和圣诞节？因为 Oct 31 等于 Dec 25。" },
        /* DEMO-DIALOG 4)：第二条笑话（避免与第一条重复；整句匹配） */
        { { "再讲一个", "再来一个", "再来一个笑话", "再讲一个笑话",
            "换个笑话", NULL },
          "我朋友说他减肥成功了，我问他怎么做到的，他说：我把体重秤调了。" },
        { { "我有点累", "有点累", "好累", "累了", "想休息", NULL },
          "累的话就先停下来歇一会儿吧，我陪着你。" },
        { { NULL, NULL, NULL, NULL, NULL, NULL }, NULL },
    };
    int i;

    if (channel == NULL || text == NULL)
      {
        return NULL;
      }

    if (strcmp(channel, AGENT_CHAN_VOICE) != 0
        && strcmp(channel, AGENT_CHAN_CLI) != 0
        && strcmp(channel, "ble") != 0)
      {
        return NULL;
      }

    /* DEMO-DIALOG：动态秒答优先（人格切换 / 步数 / 心率 / 提醒）。
     * 这些需要读设备状态或写 cron，放不进静态短语表。 */
    {
        char* dynamic_reply = demo_dynamic_reply(channel, chat_id, text);

        if (dynamic_reply != NULL)
          {
            return dynamic_reply;
          }
    }

    for (i = 0; demo_replies[i].reply != NULL; i++)
      {
        int k;

        for (k = 0; k < 6 && demo_replies[i].keywords[k] != NULL; k++)
          {
            if (demo_text_is(text, demo_replies[i].keywords[k]))
              {
                s_fast_path_count++;
                syslog(LOG_INFO,
                       "[demo-fast] chan=%s kw=\"%s\" -> local reply "
                       "(%d bytes, fast=%d llm=%d)\n",
                       channel, demo_replies[i].keywords[k],
                       (int)strlen(demo_replies[i].reply),
                       s_fast_path_count, s_llm_path_count);
                return strdup(demo_replies[i].reply);
              }
          }
      }

    return NULL;
}
#endif /* DEMO_FAST_REPLY */

static char* handle_nl_fast_path(const char* text)
{
    /* 麦克风自检先看: 它的关键词不进 s_intents 表, 需要自定义回复文案 */
    {
        char* mic = handle_mic_selfcheck_fast_path(text);
        if (mic) {
            return mic;
        }
    }

    /* v106 S5：情绪词优先（本地规则，秒出） */
    if (contains_any(text, kw_mood)) {
        static int mood_idx;
        const char* mr = mood_replies[mood_idx % 3];
        mood_idx++;
        s_fast_path_count++;
        syslog(LOG_INFO, "[%s] NL fast path: mood (fast=%d llm=%d)\n",
               TAG, s_fast_path_count, s_llm_path_count);
        return strdup(mr);
    }

    /* Table-driven: scan intents, first match wins */
    for (int i = 0; s_intents[i].keywords; i++) {
        if (contains_any(text, s_intents[i].keywords)) {
            char* reply = calloc(1, s_intents[i].output_size);
            if (reply) {
                tool_registry_execute(s_intents[i].tool_name,
                    s_intents[i].tool_args, reply,
                    s_intents[i].output_size);
            }
            if (reply) {
                reply = humanize_tool_reply(s_intents[i].tool_name, reply);
                if (reply != NULL && reply[0] == '\0') {
                    /* v108：工具空输出防呆，避免空 FINAL → 空气泡 + speak -EINVAL */
                    free(reply);
                    reply = strdup("没拿到数据，稍后再试。");
                }
                s_fast_path_count++;
                syslog(LOG_INFO,
                    "[%s] NL fast path: %s (fast=%d llm=%d)\n",
                    TAG, s_intents[i].tool_name,
                    s_fast_path_count, s_llm_path_count);
            }
            return reply;
        }
    }

    /* Skill list (special: no tool call) */
    static const char* kw_skill[] = {
        "技能列表", "有什么技能", "list skill", NULL };
    if (contains_any(text, kw_skill)) {
        char buf[2048];
        size_t n = skill_loader_build_summary(buf, sizeof(buf));
        s_fast_path_count++;
        return n > 0 ? strdup(buf) : strdup("暂无已加载的技能。");
    }

    /* Weather (special: needs city extraction) */
    static const char* kw_weather[] = {
        "天气怎么样", "weather", "天气如何", NULL };
    if (contains_any(text, kw_weather)) {
        char safe_city[64];
        strncpy(safe_city, "Beijing", sizeof(safe_city) - 1);
        safe_city[sizeof(safe_city) - 1] = '\0';

        /* 取"天气"之前的文字当地名 —— 但 "今天天气怎么样" 会把"今天"当前
         * 城市(真机 2026-10-26 日志: Weather query: 今天)。时间词/疑问词
         * 一律丢弃, 回退固定北京(手表没有定位, 数据源也只有北京坐标)。 */
        const char* p = strstr(text, "天气");
        if (p && p > text && (size_t)(p - text) <= sizeof(safe_city) - 1) {
            char cand[64];
            int ci = 0;
            for (const char* s = text; s < p && ci < 62; s++) {
                if (*s != '"' && *s != '\\' && *s != ' ')
                    cand[ci++] = *s;
            }
            cand[ci] = '\0';

            static const char* k_bad[] = {
                "今天", "明天", "后天", "现在", "今日", "早上", "上午",
                "中午", "下午", "晚上", "夜里", "最近", "这边", "那里",
                NULL };
            int bad = (ci == 0);
            for (int k = 0; !bad && k_bad[k]; k++) {
                if (strstr(cand, k_bad[k])) bad = 1;
            }
            if (!bad) {
                strncpy(safe_city, cand, sizeof(safe_city) - 1);
                safe_city[sizeof(safe_city) - 1] = '\0';
            }
        }

        char input[256];
        snprintf(input, sizeof(input),
            "{\"location\":\"%s\"}", safe_city);
        char* reply = calloc(1, 4096);
        if (reply)
            tool_registry_execute("get_weather", input, reply, 4096);
        if (reply) {
            s_fast_path_count++;
            syslog(LOG_INFO,
                "[%s] NL fast path: get_weather (fast=%d llm=%d)\n",
                TAG, s_fast_path_count, s_llm_path_count);
        }
        return reply;
    }

    /* Music play (special: needs keyword extraction) */
    static const char* kw_play[] = { "播放", "play ", "放一首", NULL };
    if (contains_any(text, kw_play)) {
        const char* kw = NULL;
        const char* p = strstr(text, "播放");
        if (p) { p += strlen("播放"); while (*p == ' ') p++; if (*p) kw = p; }
        if (!kw) {
            p = strcasestr(text, "play ");
            if (p) { p += 5; while (*p == ' ') p++; if (*p) kw = p; }
        }
        if (kw) {
            int klen = 0;
            while (kw[klen] && kw[klen] != '"' && kw[klen] != '\\' && klen < 100)
                klen++;
            char input[256];
            snprintf(input, sizeof(input),
                "{\"keyword\":\"%.*s\"}", klen, kw);
            char search_result[4096];
            memset(search_result, 0, sizeof(search_result));
            tool_registry_execute("music_search", input, search_result, sizeof(search_result));

            /* Auto-play first result if search succeeded */
            char* reply = NULL;
            cJSON* sr = cJSON_Parse(search_result);
            cJSON* songs = sr ? cJSON_GetObjectItem(sr, "songs") : NULL;
            cJSON* first = songs ? cJSON_GetArrayItem(songs, 0) : NULL;
            cJSON* url = first ? cJSON_GetObjectItem(first, "url") : NULL;
            if (url && cJSON_IsString(url) && url->valuestring[0]) {
                char play_input[512];
                snprintf(play_input, sizeof(play_input),
                    "{\"url\":\"%s\"}", url->valuestring);
                char play_result[256];
                memset(play_result, 0, sizeof(play_result));
                tool_registry_execute("music_play", play_input, play_result, sizeof(play_result));

                /* Build reply with song info */
                cJSON* name = cJSON_GetObjectItem(first, "name");
                cJSON* artist = cJSON_GetObjectItem(first, "artist");
                const char* sname = (name && cJSON_IsString(name)) ? name->valuestring : "未知";
                const char* sartist = (artist && cJSON_IsString(artist)) ? artist->valuestring : "未知";

                /* Check if play actually succeeded */
                bool play_ok = (strstr(play_result, "error") == NULL && play_result[0] != '\0');
                reply = calloc(1, 512);
                if (reply) {
                    if (play_ok) {
                        snprintf(reply, 512, "正在播放: %s - %s", sname, sartist);
                    } else {
                        snprintf(reply, 512, "找到了 %s - %s，但播放失败: %s",
                            sname, sartist, play_result);
                    }
                }
            } else {
                reply = calloc(1, 4096);
                if (reply)
                    strncpy(reply, search_result, 4095);
            }
            cJSON_Delete(sr);

            if (reply) {
                s_fast_path_count++;
                syslog(LOG_INFO,
                    "[%s] NL fast path: music_search (fast=%d llm=%d)\n",
                    TAG, s_fast_path_count, s_llm_path_count);
            }
            return reply;
        }
    }

    return NULL;
}

/* ── Handle slash commands + NL fast path (bypass LLM) ─── */

static char* handle_slash_command(agent_msg_t* msg)
{
    if (!msg->content) {
        return NULL;
    }

    /* Phase 1: slash commands (highest priority) */
    if (msg->content[0] != '/') {
        return NULL; /* NL fast path handled separately after injection check */
    }

    char* reply = NULL;

    if (strncmp(msg->content, "/help", 5) == 0) {
        reply = strdup(
            "AI Agent 快捷命令：\n\n"
            "/help           显示本帮助\n"
            "/time           当前时间\n"
            "/weather 城市   实时天气\n"
            "/news [关键词]  最新新闻\n"
            "/memory         查看长期记忆\n"
            "/note 内容      记一条笔记\n"
            "/remind 秒数 内容  设提醒\n"
            "/skill          列出技能\n"
            "/translate 文本 翻译\n"
            "/daily          每日简报\n\n"
            "也可以直接用自然语言对话，我会自动调用工具。");
    } else if (strncmp(msg->content, "/time", 5) == 0) {
        reply = calloc(1, 256);
        if (reply) {
            tool_registry_execute("get_current_time", "{}", reply, 256);
        }
    } else if (strncmp(msg->content, "/weather", 8) == 0) {
        const char* arg = msg->content + 8;
        while (*arg == ' ') {
            arg++;
        }
        if (!*arg) {
            arg = "Beijing";
        }
        char input[256];
        snprintf(input, sizeof(input),
            "{\"location\":\"%s\"}", arg);
        reply = calloc(1, 4096);
        if (reply) {
            tool_registry_execute("get_weather", input, reply, 4096);
        }
    } else if (strncmp(msg->content, "/news", 5) == 0) {
        const char* arg = msg->content + 5;
        while (*arg == ' ') {
            arg++;
        }
        if (!*arg) {
            arg = "today";
        }
        char input[256];
        snprintf(input, sizeof(input),
            "{\"query\":\"%s\",\"top_headlines\":true}", arg);
        reply = calloc(1, 8192);
        if (reply) {
            tool_registry_execute("news_search", input, reply, 8192);
        }
    } else if (strncmp(msg->content, "/memory", 7) == 0) {
        char mem_path[256];
        snprintf(mem_path, sizeof(mem_path),
            "{\"path\":\"%s/memory/MEMORY.md\"}", AGENT_DATA_DIR);
        reply = calloc(1, 4096);
        if (reply) {
            int r = tool_registry_execute(
                "read_file", mem_path, reply, 4096);
            if (r != OK || !reply[0]) {
                snprintf(reply, 4096, "暂无长期记忆。");
            }
        }
    } else if (strncmp(msg->content, "/note ", 6) == 0) {
        reply = handle_slash_note(msg);
    } else if (strncmp(msg->content, "/remind ", 8) == 0) {
        reply = handle_slash_remind(msg);
    } else if (strncmp(msg->content, "/skill", 6) == 0) {
        char skills_buf[2048];
        size_t slen = skill_loader_build_summary(
            skills_buf, sizeof(skills_buf));
        reply = (slen > 0)
            ? strdup(skills_buf)
            : strdup("暂无已加载的技能。");
    } else if (strncmp(msg->content, "/translate ", 11) == 0) {
        /* Rewrite content to pass through LLM */
        char* nc = malloc(strlen(msg->content) + 64);
        if (nc) {
            snprintf(nc, strlen(msg->content) + 64,
                "请翻译以下内容（中英互译）：%s", msg->content + 11);
            free(msg->content);
            msg->content = nc;
        }
    } else if (strncmp(msg->content, "/daily", 6) == 0) {
        char* nc = strdup(
            "请给我一份今日简报，包括：当前时间、今日天气、"
            "最新新闻摘要。");
        if (nc) {
            free(msg->content);
            msg->content = nc;
        }
    }

    if (reply) {
        s_fast_path_count++;
    }
    return reply;
}

/* /note sub-handler */
static char* handle_slash_note(const agent_msg_t* msg)
{
    const char* note = msg->content + 6;
    time_t now = time(NULL);
    struct tm tm_now;
    time_t local_epoch = now + 8 * 3600;

    gmtime_r(&local_epoch, &tm_now);

    char date_str[16];
    strftime(date_str, sizeof(date_str), "%Y-%m-%d", &tm_now);
    char time_str[16];
    strftime(time_str, sizeof(time_str), "%H:%M", &tm_now);

    char path[256];
    snprintf(path, sizeof(path), "%s/memory/daily/%s.md",
        AGENT_DATA_DIR, date_str);

    char input[4096];
    snprintf(input, sizeof(input),
        "{\"path\":\"%s\",\"content\":\"%s %s\\n- %s %s\\n\"}",
        path, "# ", date_str, time_str, note);

    char* reply = calloc(1, 512);
    if (reply) {
        tool_registry_execute("write_file", input, reply, 512);
        snprintf(reply, 512, "已记录：%s", note);
    }
    return reply;
}

/* /remind sub-handler */
static char* handle_slash_remind(const agent_msg_t* msg)
{
    int secs = 0;
    char remind_msg[512];

    remind_msg[0] = '\0';
    if (sscanf(msg->content + 8, "%d %511[^\n]",
            &secs, remind_msg)
        < 1) {
        return NULL;
    }
    if (!remind_msg[0]) {
        strncpy(remind_msg, "提醒时间到", sizeof(remind_msg) - 1);
    }

    char input[1024];
    snprintf(input, sizeof(input),
        "{\"name\":\"remind\",\"schedule_type\":\"at\","
        "\"at_epoch\":%lld,\"message\":\"%s\","
        "\"channel\":\"%s\",\"chat_id\":\"%s\"}",
        (long long)(time(NULL) + secs), remind_msg,
        msg->channel, msg->chat_id);

    char* reply = calloc(1, 512);
    if (reply) {
        tool_registry_execute("cron_add", input, reply, 512);
        snprintf(reply, 512, "好的，%d 秒后提醒你：%s",
            secs, remind_msg);
    }
    return reply;
}

/* ── Extracted: handle vision message ──────────────────────── */

static char* handle_vision_message(agent_msg_t* msg)
{
    if (!msg->image_b64 || !msg->image_b64[0]) {
        return NULL;
    }

    syslog(LOG_INFO,
        "[%s] Vision message detected, calling llm_chat_vision\n", TAG);

    size_t vis_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE, TOOL_OUTPUT_SIZE_MIN);
    char* vision_resp = calloc(1, vis_size);

    if (!vision_resp) {
        free(msg->image_b64);
        msg->image_b64 = NULL;
        return NULL;
    }

    const char* prompt = (msg->content && msg->content[0])
        ? msg->content
        : AGENT_VISION_DEFAULT_PROMPT;
    int err = llm_chat_vision(
        prompt, msg->image_b64, NULL, vision_resp, vis_size);

    free(msg->image_b64);
    msg->image_b64 = NULL;

    if (err == OK && vision_resp[0]) {
        return vision_resp;
    }

    free(vision_resp);
    return NULL;
}

/* ── Extracted: strip leaked tool-call XML markup ─────────── */

static char* strip_tool_call_markup(char* text)
{
    if (!text) {
        return NULL;
    }

    int dirty = 0;
    if (strstr(text, "<tool_call>") || strstr(text, ":tool_call>")) {
        dirty = 1;
    }
    if (!dirty) {
        return text;
    }

    syslog(LOG_WARNING,
        "[%s] Force-finish text contains tool-call markup, stripping\n",
        TAG);

    size_t len = strlen(text);
    char* clean = calloc(1, len + 1);
    if (!clean) {
        return text;
    }

    const char* r = text;
    char* w = clean;

    while (*r) {
        const char* ts = NULL;
        const char* te = NULL;

        const char* plain = strstr(r, "<tool_call>");
        const char* ns = strstr(r, ":tool_call>");
        const char* ns_open = NULL;

        if (ns) {
            ns_open = ns;
            while (ns_open > r && *(ns_open - 1) != '<') {
                ns_open--;
            }
            if (ns_open > r) {
                ns_open--;
            } else {
                ns_open = NULL;
            }
        }

        if (plain && (!ns_open || plain <= ns_open)) {
            ts = plain;
            te = strstr(ts + 11, "</tool_call>");
            if (te) {
                te += 12;
            }
        } else if (ns_open) {
            ts = ns_open;
            size_t plen = (size_t)(ns - (ts + 1));
            if (plen > 0 && plen < 32) {
                char ctag[64];
                snprintf(ctag, sizeof(ctag), "</%.*s:tool_call>",
                    (int)plen, ts + 1);
                te = strstr(ts, ctag);
                if (te) {
                    te += strlen(ctag);
                }
            }
        }

        if (ts && te) {
            size_t prefix = (size_t)(ts - r);
            memcpy(w, r, prefix);
            w += prefix;
            r = te;
        } else {
            size_t rest = strlen(r);
            memcpy(w, r, rest);
            w += rest;
            break;
        }
    }
    *w = '\0';

    free(text);
    if (clean[0] == '\0' || strspn(clean, " \t\r\n") == strlen(clean)) {
        free(clean);
        return strdup(
            "抱歉，这个任务比较复杂，"
            "我已经收集了一些信息但未能完成全部步骤。"
            "请尝试拆分成更小的问题再问我。");
    }
    return clean;
}

/* ── Extracted: force finish when iteration limit reached ─── */

static char* force_finish_reply(const char* system_prompt,
    cJSON* messages)
{
    syslog(LOG_WARNING,
        "[%s] Tool iteration limit (%d) reached, forcing finish\n",
        TAG, AGENT_AI_AGENT_MAX_TOOL_ITER);

    cJSON* hint = cJSON_CreateObject();
    cJSON_AddStringToObject(hint, "role", "system");
    cJSON_AddStringToObject(hint, "content",
        "You have used all available tool iterations. "
        "Do NOT call any more tools. Summarize what you have "
        "learned so far and reply to the user in plain text now.");
    cJSON_AddItemToArray(messages, hint);

    llm_response_t resp;
    char* result = NULL;
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    int err = llm_chat_tools(system_prompt, messages, NULL, &resp);
    gettimeofday(&t1, NULL);
    uint32_t ms = calc_elapsed_ms(&t0, &t1);
    bool timed_out = llm_call_timed_out(ms);

    if (err == OK && !timed_out && resp.text && resp.text_len > 0) {
        result = strdup(resp.text);
    }
    llm_response_free(&resp);

    if (!result && timed_out) {
        result = strdup(LLM_TIMEOUT_MSG);
    }

    return strip_tool_call_markup(result);
}

/* ── Extracted: dispatch response to outbound bus ─────────── */

/* Set by run_react_loop when the answer came from the on-device model, so the
 * reply can be labelled on the phone page and on the watch.  The agent loop
 * handles one message at a time, so a plain static is enough. */
static bool s_answer_from_local;

static void dispatch_response(const agent_msg_t* msg,
    char* final_text)
{
    if (final_text && final_text[0]) {
        syslog(LOG_INFO, "[%s] probe: append_user_begin\n", TAG);
        /* v114b2：暂停会话持久化写 /data(NOR/littlefs)。
         * 依据 B 报告：trace END 后只做两次 session_append 即整机停摆，且重启后
         * 出现 NOR 写校验失败；flash 写在 up_irq_save 内无超时。演示期先不落盘。 */
        syslog(LOG_INFO, "[%s] probe: append_user_skipped (persist disabled)\n", TAG);
        syslog(LOG_INFO, "[%s] probe: append_asst_begin\n", TAG);
        syslog(LOG_INFO, "[%s] probe: append_asst_skipped (persist disabled)\n", TAG);

        agent_msg_t out = { 0 };
        strncpy(out.channel, msg->channel,
            sizeof(out.channel) - 1);
        strncpy(out.chat_id, msg->chat_id,
            sizeof(out.chat_id) - 1);
        out.content = final_text;
        out.from_local = s_answer_from_local;
        /* 真正的回答：只有这一类允许被念出来 (P2a)。 */
        out.kind = AGENT_MSG_KIND_FINAL;
        syslog(LOG_INFO, "[%s] probe: push_begin\n", TAG);
        if (message_bus_push_outbound(&out) != OK) {
            free(final_text);
        }
        syslog(LOG_INFO, "[%s] probe: push_done\n", TAG);
    } else {
        free(final_text);
        agent_msg_t out = { 0 };
        strncpy(out.channel, msg->channel,
            sizeof(out.channel) - 1);
        strncpy(out.chat_id, msg->chat_id,
            sizeof(out.chat_id) - 1);
        /* 中文诚实兜底。英文报错直接显示在手表屏幕上是演示事故
         * (2026-10-25 现场: 关怀 L2 失败时屏幕上出现
         * "Sorry, I encountered an error.")。 */
        out.content = strdup("抱歉，这个我暂时答不上来，等会儿再问我一次吧。");
        /* 错误提示不是回答：上屏/上手机，但绝不进 TTS。 */
        out.kind = AGENT_MSG_KIND_ERROR;
        if (out.content) {
            if (message_bus_push_outbound(&out) != OK) {
                free(out.content);
            }
        }
    }
}

/* ── ReAct tool-calling loop ──────────────────────────────── */

static const char* s_working_phrases[] = {
    "正在思考中...",
    "稍等，处理中...",
    "让我查一下...",
    "正在分析...",
    "马上好...",
};
#define WORKING_PHRASE_COUNT \
    ((int)(sizeof(s_working_phrases) / sizeof(s_working_phrases[0])))

/* Send a "working" status message on the first iteration.
 * Skip for feishu (has its own typing indicator) and voice
 * (TTS synthesis of a status phrase wastes time and memory).
 * P2a: 状态语仍然上屏(手机页的"typing"提示 / 手表气泡), 只是不念 ——
 * 靠下面的 kind=STATUS 标记过滤 TTS, 不靠把通道塞进这个跳过名单。 */
static void send_working_status(const agent_msg_t* msg, int iteration)
{
    if (iteration != 0) {
        return;
    }
    if (strcmp(msg->channel, AGENT_CHAN_FEISHU) == 0
        || strcmp(msg->channel, AGENT_CHAN_VOICE) == 0
#ifdef CONFIG_FEATURE_SYSTEM_VELACLAW
        || strcmp(msg->channel, AGENT_CHAN_QUICKAPP) == 0
#endif
        || strcmp(msg->channel, AGENT_CHAN_WEIXIN) == 0) {
        return;
    }

    agent_msg_t status = { 0 };

    strncpy(status.channel, msg->channel, sizeof(status.channel) - 1);
    strncpy(status.chat_id, msg->chat_id, sizeof(status.chat_id) - 1);
    /* 进度语，不是回答：speak 侧只认 FINAL (P2a)。 */
    status.kind = AGENT_MSG_KIND_STATUS;
    status.content = strdup(
        s_working_phrases[(unsigned)rand() % WORKING_PHRASE_COUNT]);
    if (status.content) {
        if (message_bus_push_outbound(&status) != OK) {
            free(status.content);
        }
    }
}

/* Check for duplicate tool calls. Returns true if loop should break. */
static bool check_tool_dup(const llm_response_t* resp,
    char* prev_sig, int* dup_count,
    char* prev_name, int* name_repeat)
{
    if (resp->call_count != 1) {
        prev_sig[0] = '\0';
        *dup_count = 0;
        prev_name[0] = '\0';
        *name_repeat = 0;
        return false;
    }

    /* Exact match (name + args) */
    char cur_sig[512];

    snprintf(cur_sig, sizeof(cur_sig), "%s|%.400s",
        resp->calls[0].name,
        resp->calls[0].input ? resp->calls[0].input : "");

    if (strcmp(cur_sig, prev_sig) == 0) {
        (*dup_count)++;
        if (*dup_count >= 2) {
            syslog(LOG_WARNING,
                "[%s] Duplicate tool call detected (%s), breaking\n",
                TAG, resp->calls[0].name);
            return true;
        }
    } else {
        *dup_count = 0;
    }
    strncpy(prev_sig, cur_sig, 511);
    prev_sig[511] = '\0';

    /* Name-only repeat detection */
    if (strcmp(resp->calls[0].name, prev_name) == 0) {
        (*name_repeat)++;
        if (*name_repeat >= AGENT_TOOL_NAME_REPEAT_MAX) {
            syslog(LOG_WARNING,
                "[%s] Tool name repeat limit (%s called %d times)\n",
                TAG, resp->calls[0].name, *name_repeat + 1);
            return true;
        }
    } else {
        *name_repeat = 0;
    }
    strncpy(prev_name, resp->calls[0].name, 63);
    prev_name[63] = '\0';

    return false;
}

/* Check if an LLM call exceeded the watchdog timeout.
 * Returns true when latency_ms exceeds AGENT_LLM_TIMEOUT_SEC. */
static bool llm_call_timed_out(uint32_t latency_ms)
{
    return latency_ms > (uint32_t)AGENT_LLM_TIMEOUT_SEC * 1000;
}

/* Handle TASK_COMPLETE: inject hint and do one final LLM call.
 * If the LLM call times out, *out_timed_out is set to true. */
static char* handle_task_complete(const char* sys_prompt, cJSON* messages,
    bool* out_timed_out)
{
    cJSON* hint = cJSON_CreateObject();

    cJSON_AddStringToObject(hint, "role", "system");
    cJSON_AddStringToObject(hint, "content",
        "The task has been completed successfully. "
        "Do NOT call any more tools. "
        "Reply to the user now confirming what was done.");
    cJSON_AddItemToArray(messages, hint);

    llm_response_t final_resp;
    char* result = NULL;
    struct timeval t0, t1;
    gettimeofday(&t0, NULL);
    int err = llm_chat_tools(sys_prompt, messages, NULL, &final_resp);
    gettimeofday(&t1, NULL);
    uint32_t ms = calc_elapsed_ms(&t0, &t1);
    bool timed_out = llm_call_timed_out(ms);

    if (err == OK && !timed_out
        && final_resp.text && final_resp.text_len > 0) {
        result = strdup(final_resp.text);
    }
    llm_response_free(&final_resp);

    if (!result && timed_out) {
        if (out_timed_out) {
            *out_timed_out = true;
        }
        return strdup(LLM_TIMEOUT_TASK_COMPLETE_MSG);
    }
    return result;
}

/* 构造 LLM 响应缓存键：人格 id + 用户文本。
 *
 * 缓存值只按这个键做 djb2（前 LLM_CACHE_KEY_LEN 字节），所以人格必须在
 * 前几位，否则切换人格后同一句话仍会命中旧人格的回复。 */
static void build_llm_cache_key(char* key, size_t size,
                                persona_id_t persona, const char* content)
{
    snprintf(key, size, "%d:%s", (int)persona,
        content ? content : "");
}

/* 2026-11-03 SPEED-A：本轮（run_react_loop）流式统计，供 agent_loop_task
 * 的 [turn] 日志打印 first_token_ms / stream_deltas（验收用）。
 * agent_loop 是单线程循环，用普通静态变量即可。
 *   deltas = 本轮所有流式 LLM 调用收到的非空 delta 总数（工具轮失败重发会累加）
 *   first_ms = 本轮第一次"LLM 请求开始 → 首个 delta"的毫秒数；-1 表示未流式
 */
static int s_turn_stream_deltas;
static long s_turn_stream_first_ms = -1;

#ifdef CONFIG_AI_AGENT_LVGL_UI
/* 2026-11-03 SPEED-A 最小集成：把 LLM 的 SSE 增量回答实时投到手表屏幕。
 * 只负责“生成过程中先看见文字”；最终回答仍走 outbound 镜像（历史/气泡一致），
 * 因此在 LLM 返回后立刻 abort 掉流式占位，避免与最终消息重复。 */
typedef struct {
    const char* user_text;
    const char* channel;
    bool  started;
    bool  first;
    int   deltas;
    long  first_ms;
    struct timeval t0;
    /* VOICE-B（docs_ble/112 §2B）：voice 通道的流式首句 TTS。
     * 首个 delta 前 begin，后续 delta feed，最终回答处 end（takeover 防重复）。 */
    bool  voice;
    bool  voice_started;
} agent_stream_ctx_t;

static void agent_llm_stream_delta(const char* delta, void* arg)
{
    agent_stream_ctx_t* c = (agent_stream_ctx_t*)arg;
    struct timeval tv;

    if (!c || !delta || delta[0] == '\0') {
        return;
    }

    if (!c->first) {
        c->first = true;
        gettimeofday(&tv, NULL);
        c->first_ms = (long)calc_elapsed_ms(&c->t0, &tv);
    }

    if (!c->started) {
        if (lvgl_ui_channel_stream_begin(c->user_text) == 0) {
            c->started = true;
        }
    }

    if (c->started) {
        (void)lvgl_ui_channel_stream_delta(delta);
    }

    /* VOICE-B：voice 通道边生成边播首句。begin/feed 只做短内存操作 +
     * speak 队列入队（首句凑齐即异步入队），不在网络回调里做 TTS/播放。 */
    if (c->voice) {
        if (!c->voice_started) {
            voice_channel_stream_begin();
            c->voice_started = true;
        }
        voice_channel_stream_feed(delta);
    }
    c->deltas++;
}
#endif

/* LOCAL-FAST C：允许"本地快路径优先"的用户对话通道白名单。
 * 只抢 voice/ble/cli 的真实用户对话；care/system/heartbeat 等主动关怀、
 * 系统 meta-prompt 通道一律不抢（它们内容长，本地模板本来也不命中）。 */
static bool agent_channel_is_user_dialog(const char* channel)
{
    return channel != NULL
        && (strcmp(channel, AGENT_CHAN_VOICE) == 0
            || strcmp(channel, "ble") == 0
            || strcmp(channel, AGENT_CHAN_CLI) == 0);
}

/* Run the ReAct tool-calling loop. Returns final_text (caller owns). */
static char* run_react_loop(const char* sys_prompt, cJSON* messages,
    const char* tools_json, char* tool_output, size_t tool_size,
    const agent_msg_t* msg)
{
    char prev_sig[512];
    int dup_count;
    char prev_name[64];
    int name_repeat;
    int iteration;
    char* final_text = NULL;

    s_answer_from_local = false;   /* cleared per request; set on local fallback */
    prev_sig[0] = '\0';
    dup_count = 0;
    prev_name[0] = '\0';
    name_repeat = 0;
    int last_total_tokens = 0;
    bool watchdog_fired = false;
    persona_id_t cache_persona;

    /* LOCAL-FAST B-FIX（perf-fw 复核 P0）：
     *   control_intent —— 设备控制类消息禁止交给端侧模型（ai_lm 的虚拟设备
     *     执行器会编造“已为您打开/已上锁”，而真实硬件无动作）。
     *   local_attempted —— 离线本地优先是否已经试过一次；断网时原兜底分支
     *     不要对同一条消息再跑一次端侧推理（复核 P0：离线最坏等待翻倍）。 */
    bool control_intent = (msg->content != NULL)
        && local_lm_is_control_intent(msg->content);
    bool local_attempted = false;

    s_turn_stream_deltas = 0;
    s_turn_stream_first_ms = -1;

#ifdef CONFIG_AI_AGENT_LVGL_UI
    agent_stream_ctx_t stream_ctx;
    bool stream_enabled;

    memset(&stream_ctx, 0, sizeof(stream_ctx));
    stream_ctx.user_text = msg->content;
    stream_ctx.channel = msg->channel;
    stream_ctx.voice = (strcmp(msg->channel, AGENT_CHAN_VOICE) == 0);
    stream_enabled = (msg->channel[0] != '\0')
        && (strcmp(msg->channel, "voice") == 0
            || strcmp(msg->channel, "ble") == 0
            || strcmp(msg->channel, "cli") == 0);
#endif

    /* Router: select and apply best backend before first LLM call.
     * Estimate complexity from the last user message. */
    llm_complexity_t complexity = LLM_COMPLEXITY_SIMPLE;
    if (msg->content) {
        complexity = llm_router_estimate_complexity(
            msg->content, strlen(msg->content));
    }
    int router_idx = llm_router_select(complexity);
    if (router_idx >= 0) {
        llm_router_apply(router_idx);
    }

    /* Structured trace: begin */
    agent_trace_t trace;
    agent_trace_begin(&trace, msg->chat_id, msg->channel);
    trace.backend_idx = router_idx;

    /* Cache check: for simple queries, try cache before LLM call.
     *
     * 缓存键必须带人格：llm_cache 只按用户文本做 djb2，若把人格漏掉，
     * 切到毒舌后再问同一句话会命中温柔人格的缓存（TTL 300s），
     * 「同一句话三种风格」的演示就失效了。
     *
     * 用本轮开始时的快照：set_persona 可能在本轮工具调用中改了全局状态，
     * 但这条回复仍由旧 prompt 生成，缓存 get/put 必须用同一个人格 id。 */
    cache_persona = persona_get();

    if (msg->content && complexity == LLM_COMPLEXITY_SIMPLE) {
        char cache_key[LLM_CACHE_KEY_LEN + 8];
        char* cached;

        build_llm_cache_key(cache_key, sizeof(cache_key), cache_persona,
            msg->content);
        cached = llm_cache_get(cache_key, strlen(cache_key));
        if (cached) {
            syslog(LOG_INFO, "[%s] Cache hit, skipping LLM call\n", TAG);
            final_text = cached;
            agent_trace_step(&trace, 0, NULL, 0, 1);
            agent_trace_end(&trace, AGENT_TRACE_OK);
            goto send_reply;
        }
    }

#ifdef CONFIG_AI_AGENT_LVGL_UI
    /* 2026-11-03：不等第一个 delta —— 回合一开始先切 AI 页并显示
     * 小云思考中…，让用户立刻看到反馈；后续 delta 再追加到同一气泡。 */
    if (stream_enabled) {
        gettimeofday(&stream_ctx.t0, NULL);
        stream_ctx.first = false;
        stream_ctx.deltas = 0;
        if (lvgl_ui_channel_stream_begin(msg->content) == 0) {
            stream_ctx.started = true;
        }
    }
#endif

    for (iteration = 0; iteration < AGENT_AI_AGENT_MAX_TOOL_ITER;
         iteration++) {
        send_working_status(msg, iteration);

        /* ★ 必须清零再用。
         *
         * 2026-10-20 真机定位的崩溃：断网时 network_get_state() 直接判错，
         * llm_chat_tools() **根本没被调用**，resp 里是栈上的垃圾；而下面的
         * 失败路径照样执行 llm_response_free(&resp)，于是 free() 了一个
         * 不属于堆的指针，撞上 mm_free.c:315 的
         *     DEBUGASSERT(mm_heapmember(heap, mem))
         * 整个 ai_agent 任务被 assert 打死并复位。
         *
         * 触发条件很常见：没有网络 + 主动关怀(pet_care L2)调用 LLM —— 也就是
         * 手表离了手机几分钟后必崩。清零之后 llm_response_free() 释放的是
         * 一堆 NULL，安全。 */
        llm_response_t resp;
        memset(&resp, 0, sizeof(resp));
        struct timeval tv_start, tv_end;
        bool uplink_down = false;
        int err;

        /* ── LOCAL-FAST C（2026-11-04）：简单消息本地快路径优先 ──
         * 实测问题：离线时 network 状态没及时翻转，`ask 你好` 仍先等云端
         * 失败 ~16s 才走端侧快路径。这里在**第 0 轮、云 LLM 之前**先试 A 的
         * 模板/规则快路径（毫秒级、不跑模型、不联网），命中即本地回答并
         * break，完全跳过云端（无论在线/离线）。
         * 未命中 / 控制意图 / 非用户对话通道（care/system/heartbeat）→ 继续
         * 原路径：联网走云端，离线走下面的"离线本地优先/断网直落端侧"。
         * 本地回答不写 LLM cache（send_reply 已按 s_answer_from_local 跳过）。 */
#if LOCAL_FAST_PREEMPT
        if (iteration == 0 && !control_intent
            && msg->content != NULL
            && agent_channel_is_user_dialog(msg->channel)) {
            char* fast_text = NULL;
            int64_t lm_t0 = agent_lat_mono_ms();
            int lm_ret = local_lm_fast_reply(msg->content, &fast_text);
            long lm_ms = (long)(agent_lat_mono_ms() - lm_t0);

            /* RCA-FIX-A F8：local_lm 快路径耗时（模板命中/未命中都记） */
            syslog(LOG_INFO,
                "[%s] [lat] local_lm fast ms=%ld ret=%d hit=%d\n",
                TAG, lm_ms, lm_ret,
                (lm_ret == 0 && fast_text != NULL && fast_text[0] != '\0'));

            if (lm_ret == 0
                && fast_text != NULL && fast_text[0] != '\0') {
                final_text = fast_text;
                s_answer_from_local = true;
#ifdef CONFIG_AI_AGENT_LVGL_UI
                /* 回合开始已放"小云思考中…"占位：本地快路径不走流式，
                 * 收回占位，最终回答由 dispatch 镜像落屏。 */
                if (stream_enabled && stream_ctx.started) {
                    lvgl_ui_channel_stream_abort();
                    stream_ctx.started = false;
                }
#endif
                syslog(LOG_INFO,
                    "[%s] local-fast preempt: chan=%s -> %zu B template reply, "
                    "cloud skipped (no network wait)\n",
                    TAG, msg->channel, strlen(final_text));
                agent_trace_step(&trace, iteration, NULL, 0, 1);
                break;
            }
            free(fast_text);   /* 未命中/控制类：交回原路径 */
        }
#endif /* LOCAL_FAST_PREEMPT */

        /* ── LOCAL-FAST B-FIX（perf-fw 复核）：只在**离线**时本地优先 ──
         * 判据只有 network_get_state()!=NET_STATE_CONNECTED。
         * 已删除 IOB>=75% 触发：IOB 是全局网络缓冲池，TTS 下载/ASR 上传/PAN
         * 风暴都会瞬时抬高，connected+IOB 高 ≠ 云不可用；按 IOB 触发会把
         * 正常云回答降级成端侧模型（质量差、且可能编造设备动作）。
         * 控制类消息（control_intent）绝不进入 ai_lm 虚拟设备执行器：离线时
         * 给诚实的固定提示（明确不做动作）；联网时的控制请求本来就不会走到
         * 这里（本块只在离线生效），仍由云端工具链正常处理。
         * 失败：不改原路径（断网继续走下面既有“no uplink 直落端侧”分支，
         * 但 local_attempted 会阻止它对同一条消息再推理一次）。 */
#if LOCAL_FIRST_ENABLE
        if (iteration == 0
            && network_get_state() != NET_STATE_CONNECTED) {
            char* local_first_text = NULL;
            int local_first_ret = -1;
            bool local_avail = local_lm_available();

            if (control_intent) {
                /* 离线 + 设备控制：明确“需要联网”，绝不编造已执行动作。 */
                final_text = strdup(
                    "这个需要联网才能执行设备操作，等我连上手机再试～");
                local_attempted = true;   /* 原兜底也不要再跑端侧模型 */
                if (final_text != NULL) {
                    s_answer_from_local = true;
#ifdef CONFIG_AI_AGENT_LVGL_UI
                    if (stream_enabled && stream_ctx.started) {
                        lvgl_ui_channel_stream_abort();
                        stream_ctx.started = false;
                    }
#endif
                    syslog(LOG_WARNING,
                        "[%s] local-first: offline + control intent -> honest "
                        "refusal (no device action, no ai_lm takeover)\n", TAG);
                    agent_trace_step(&trace, iteration, NULL, 0, 1);
                    break;
                }
                syslog(LOG_WARNING,
                    "[%s] local-first: offline + control intent, refusal "
                    "alloc failed, falling through\n", TAG);
            } else if (local_avail && msg->content != NULL) {
                int64_t lm_t0 = agent_lat_mono_ms();

                local_attempted = true;   /* 复核 P0：防断网时二次推理 */
                local_first_ret = local_lm_reply(msg->content,
                    &local_first_text);
                /* RCA-FIX-A F8：离线本地优先的端侧耗时 */
                syslog(LOG_INFO,
                    "[%s] [lat] local_lm reply_offline ms=%ld ret=%d\n",
                    TAG, (long)(agent_lat_mono_ms() - lm_t0),
                    local_first_ret);
            }

            if (local_first_ret == 0 && local_first_text != NULL
                && local_first_text[0] != '\0') {
                final_text = local_first_text;
                s_answer_from_local = true;
#ifdef CONFIG_AI_AGENT_LVGL_UI
                /* 回合开始时已经放了“小云思考中…”占位：本地优先不走流式，
                 * 必须收回占位，最终回答由 dispatch 镜像落屏。 */
                if (stream_enabled && stream_ctx.started) {
                    lvgl_ui_channel_stream_abort();
                    stream_ctx.started = false;
                }
#endif
                syslog(LOG_INFO,
                    "[%s] local-first: offline (chan=%s) -> local reply %zu B, "
                    "cloud skipped\n",
                    TAG, msg->channel, strlen(final_text));
                agent_trace_step(&trace, iteration, NULL, 0, 1);
                break;
            }

            if (!control_intent) {
                free(local_first_text);   /* 失败/空结果：本地不接管 */
                syslog(LOG_INFO,
                    "[%s] local-first: offline (chan=%s) but local reply failed "
                    "(avail=%d ret=%d), continuing original path\n",
                    TAG, msg->channel, (int)local_avail, local_first_ret);
            }
        }
#endif /* LOCAL_FIRST_ENABLE */

        gettimeofday(&tv_start, NULL);
        if (network_get_state() != NET_STATE_CONNECTED) {
            /* No uplink: a cloud call could only hang until the socket
             * timeout and fall back anyway, so answer from the on-device
             * model right away.  Deliberately NOT counted as a backend
             * failure (llm_router_report_failure) - the backend is fine,
             * the radio is not, and penalising it would take the cloud down
             * for the whole recovery window even after the link returns. */
            syslog(LOG_WARNING,
                "[%s] no uplink, answering on-device (cloud skipped)\n", TAG);
            err = ERROR;
            uplink_down = true;
#ifdef CONFIG_AI_AGENT_LVGL_UI
            /* 回合开始已放的"思考中"占位必须收回：这条路径根本不会发起
             * 流式请求，也没有 delta 去触发 begin/end 的配对。 */
            if (stream_enabled && stream_ctx.started) {
                lvgl_ui_channel_stream_abort();
                stream_ctx.started = false;
            }
#endif
        } else {
#ifdef CONFIG_AI_AGENT_LVGL_UI
            if (stream_enabled) {
                gettimeofday(&stream_ctx.t0, NULL);
                stream_ctx.first = false;
                stream_ctx.deltas = 0;
                err = llm_chat_tools_stream(sys_prompt, messages, tools_json,
                    &resp, agent_llm_stream_delta, &stream_ctx);

                /* 只有"确定不是最终回答"的情况在这里立刻 abort：
                 *  - 调用失败（后面可能 failover / 端侧兜底）
                 *  - 工具调用轮（不显示半成品）
                 *  - 空文本（最终会走错误兜底）
                 * 最终回答的 stream_end 延后到下面 !resp.tool_use 分支：
                 * cascade 重试可能换成另一段文本，先落历史会写错一条。 */
                if (stream_ctx.started
                    && (err != OK || resp.tool_use || resp.text == NULL
                        || resp.text[0] == '\0')) {
                    lvgl_ui_channel_stream_abort();
                    stream_ctx.started = false;
                }
                /* VOICE-B：调用失败/工具轮/空文本 -> 放弃流式首句，
                 * 让 outbound dispatch 回退原整段 TTS（与旧行为一致）。 */
                if (stream_ctx.voice_started
                    && (err != OK || resp.tool_use || resp.text == NULL
                        || resp.text[0] == '\0')) {
                    voice_channel_stream_abort();
                    stream_ctx.voice_started = false;
                }
                /* 本轮累计（[turn] llm_done 行会汇总打印，验收直接 grep） */
                s_turn_stream_deltas += stream_ctx.deltas;
                if (s_turn_stream_first_ms < 0 && stream_ctx.first)
                    s_turn_stream_first_ms = stream_ctx.first_ms;

                if (stream_ctx.first) {
                    syslog(LOG_INFO,
                        "[%s] [turn] chan=%s first_token_ms=%ld stream_deltas=%d\n",
                        TAG, msg->channel, stream_ctx.first_ms,
                        stream_ctx.deltas);
                } else {
                    syslog(LOG_INFO,
                        "[%s] [turn] chan=%s stream no-delta (fallback/empty)\n",
                        TAG, msg->channel);
                }
            } else
#endif
            {
                err = llm_chat_tools(sys_prompt, messages, tools_json, &resp);
            }
        }
        gettimeofday(&tv_end, NULL);
        uint32_t latency_ms = calc_elapsed_ms(&tv_start, &tv_end);

        /* VOICE-A-FIX（perf-fw A2/P0）：-ETIMEDOUT 是首 token 降级信号，不是
         * "后端故障"——不要 failover 到下一个后端（那里会再走一次非流式
         * llm_chat_tools，最长 AGENT_LLM_TIMEOUT_SEC，离群又变回几十秒），
         * 直接落到下面的 local fallback 路径。
         * llm_proxy 已保证 -ETIMEDOUT 只可能来自语音回合的首 token 超时。 */
#if VOICE_A_ETIMEDOUT_SHORTCIRCUIT
        if (err == -ETIMEDOUT) {
            syslog(LOG_WARNING,
                "[%s] first-token timeout -> skip router failover/non-stream, "
                "go local fallback\n", TAG);
        }
        else
#endif
        /* Router failover: on LLM call failure, try next backend */
        if (err != OK && router_idx >= 0 && !uplink_down) {
            syslog(LOG_WARNING,
                "[%s] LLM call failed on backend %d, trying failover\n",
                TAG, router_idx);
            llm_router_report_failure(router_idx);

            int next_idx = llm_router_select(complexity);
            if (next_idx >= 0 && next_idx != router_idx) {
                llm_router_apply(next_idx);
                router_idx = next_idx;
                trace.backend_idx = next_idx;
                llm_response_free(&resp);
                gettimeofday(&tv_start, NULL);
                err = llm_chat_tools(sys_prompt, messages,
                    tools_json, &resp);
                gettimeofday(&tv_end, NULL);
                latency_ms = calc_elapsed_ms(&tv_start, &tv_end);
            }
        }

        if (err != OK) {
            /* Cloud is unreachable, or has no API key at all -- which is the
             * normal state on this board.  Fall back to the on-device model
             * before giving up: the user's message is right here, and a real
             * answer beats an apology.  Everything downstream (session append,
             * cache, outbound dispatch) is shared with the cloud path.
             */

            char *local_text = NULL;
            bool timed_out = llm_call_timed_out(latency_ms);

            if (err == -ETIMEDOUT && !timed_out) {
                syslog(LOG_WARNING,
                    "[%s] first-token timeout (%" PRIu32 "ms) -> local fallback "
                    "(no cloud retry)\n", TAG, latency_ms);
            }

            if (timed_out) {
                syslog(LOG_WARNING,
                    "[%s] LLM watchdog: call failed after %" PRIu32 " ms "
                    "(limit %ds)\n",
                    TAG, latency_ms, AGENT_LLM_TIMEOUT_SEC);
            } else {
                syslog(LOG_ERR, "[%s] LLM call failed (iter %d)\n",
                    TAG, iteration);
            }

            agent_trace_step(&trace, iteration, NULL, latency_ms, 0);
            llm_response_free(&resp);

            /* VOICE-F2（task-60）：语音回合在"联网但云端失败/首 token 超时/
             * 看门狗超时"时跳过 ~7s 端侧模型，直接 <=30 字短模板走 dispatch；
             * 真正离线（uplink_down / 非 CONNECTED）保留下面的 local_lm。 */
            {
                bool voice_chan =
                    (strcmp(msg->channel, AGENT_CHAN_VOICE) == 0);
                bool offline = uplink_down
                    || (network_get_state() != NET_STATE_CONNECTED);

                if (voice_chan && !offline) {
                    char* short_text = voice_f2_timeout_short_text(msg,
                        (err == -ETIMEDOUT) ? "first-token timeout"
                        : (timed_out ? "watchdog timeout"
                                     : "cloud failure while online"));
                    if (short_text != NULL) {
                        final_text = short_text;
                        s_answer_from_local = true;
                        watchdog_fired = timed_out;
                        break;
                    }
                }
            }

            /* LOCAL-FAST B-FIX（perf-fw 复核 P0）：
             *   - control_intent：控制类消息即使云端失败也不交给端侧模型
             *     （ai_lm 虚拟设备执行器会编造“已为您打开/已上锁”，真实硬件
             *     无动作）；不生成动作确认，交给下面超时/错误兜底输出诚实提示。
             *   - local_attempted：离线时上面的本地优先已经试过一次，这里
             *     不再重复推理（复核 P0：离线最坏等待翻倍）。 */
            if (control_intent) {
                syslog(LOG_WARNING,
                    "[%s] local-first: control intent -> skip on-device model "
                    "in fallback (no virtual device action)\n", TAG);
            }

            if (!local_attempted && !control_intent && local_lm_available()) {
#if VOICE_F2_TIMEOUT_SKIP_TFLM
                /* VOICE-F2：只有"真正离线"的语音回合才保留端侧模型兜底
                 * （联网超时已在上面短路成短模板）。 */
                if (strcmp(msg->channel, AGENT_CHAN_VOICE) == 0
                    && (uplink_down
                        || network_get_state() != NET_STATE_CONNECTED)) {
                    syslog(LOG_INFO,
                        "[%s] voice-f2: real offline (chan=%s) -> keep "
                        "local_lm fallback\n", TAG, msg->channel);
                }
#endif
                int64_t lm_t0 = agent_lat_mono_ms();
                int lm_ret = local_lm_reply(msg->content, &local_text);

                /* RCA-FIX-A F8：云端失败后回落端侧的耗时（本次 18:10 为 ~7s） */
                syslog(LOG_INFO,
                    "[%s] [lat] local_lm reply_fallback ms=%ld ret=%d\n",
                    TAG, (long)(agent_lat_mono_ms() - lm_t0), lm_ret);
                if (lm_ret == 0) {
                    final_text = local_text;
                    s_answer_from_local = true;   /* label the reply 本地 */
                    break;
                }
            }

            if (timed_out) {
                final_text = strdup(LLM_TIMEOUT_MSG);
                watchdog_fired = true;
            }
            break;
        }

        /* Watchdog: if the LLM call took longer than the configured
         * timeout, treat it as a timeout even if it returned OK.
         * The socket SO_RCVTIMEO may have fired and caused a partial
         * or error response that llm_chat_tools mapped to ERROR above,
         * but if the call barely completed, we still flag it. */
        if (llm_call_timed_out(latency_ms)) {
            syslog(LOG_WARNING,
                "[%s] LLM watchdog: call took %" PRIu32 " ms (limit %ds), "
                "treating as timeout\n",
                TAG, latency_ms, AGENT_LLM_TIMEOUT_SEC);
            agent_trace_step(&trace, iteration, NULL, latency_ms, 0);
            llm_response_free(&resp);
#ifdef CONFIG_AI_AGENT_LVGL_UI
            if (stream_ctx.started) {
                lvgl_ui_channel_stream_abort();
                stream_ctx.started = false;
            }
            /* VOICE-B：超时降级 -> 放弃流式首句，dispatch 播降级文案 */
            if (stream_ctx.voice_started) {
                voice_channel_stream_abort();
                stream_ctx.voice_started = false;
            }
#endif
            /* VOICE-F2：语音回合看门狗超时不跑端侧模型 -> 短模板。 */
            {
                char* short_text = voice_f2_timeout_short_text(msg,
                    "post-call watchdog");
                final_text = (short_text != NULL) ? short_text
                                                  : strdup(LLM_TIMEOUT_MSG);
            }
            watchdog_fired = true;
            break;
        }

        /* Report success to router */
        if (router_idx >= 0) {
            llm_router_report_success(router_idx);
            llm_router_report_latency(router_idx, latency_ms);
            if (resp.total_tokens > 0) {
                llm_router_report_tokens(router_idx,
                    resp.prompt_tokens, resp.completion_tokens);
                last_total_tokens = resp.total_tokens;
            }
        }

        syslog(LOG_INFO,
            "[%s] LLM resp: text=%zu, tool_use=%d, calls=%d\n",
            TAG, resp.text_len, resp.tool_use, resp.call_count);

        if (!resp.tool_use) {
            /* Cascade routing: if AUTO profile selected a cheap backend
             * for a SIMPLE query but the response looks inadequate,
             * retry once with PREMIUM tier. */
            bool cascade_retry = false;
            if (llm_router_get_profile() == LLM_ROUTE_AUTO
                && complexity == LLM_COMPLEXITY_SIMPLE
                && router_idx >= 0
                && resp.text != NULL) {
                bool low_quality = (strstr(resp.text, "I don't know") != NULL
                    || strstr(resp.text, "I cannot") != NULL
                    || strstr(resp.text, "无法") != NULL);
                if (low_quality) {
                    cascade_retry = true;
                }
            }

            if (cascade_retry) {
#ifdef CONFIG_AI_AGENT_LVGL_UI
                /* 重试会换成另一段文本：先撤销流式半成品（不落历史），
                 * 最终文本由随后 outbound 镜像落历史 —— 否则历史里会留下
                 * cheap 文本 + premium 文本两条。 */
                if (stream_ctx.started) {
                    lvgl_ui_channel_stream_abort();
                    stream_ctx.started = false;
                }
                /* VOICE-B：重试会换文本 -> 放弃流式首句，最终整段 TTS 兜底 */
                if (stream_ctx.voice_started) {
                    voice_channel_stream_abort();
                    stream_ctx.voice_started = false;
                }
#endif
                syslog(LOG_INFO,
                    "[%s] Cascade: cheap response inadequate, "
                    "retrying with PREMIUM\n",
                    TAG);

                /* Save the cheap response as fallback before freeing */
                char* fallback_text = NULL;
                if (resp.text && resp.text_len > 0) {
                    fallback_text = strdup(resp.text);
                }
                llm_response_free(&resp);

                /* Select a PREMIUM backend without changing profile */
                int prem_idx = llm_router_select_with_profile(
                    LLM_COMPLEXITY_MEDIUM, LLM_ROUTE_PREMIUM);

                if (prem_idx >= 0 && prem_idx != router_idx) {
                    llm_router_apply(prem_idx);
                    router_idx = prem_idx;
                    trace.backend_idx = prem_idx;

                    gettimeofday(&tv_start, NULL);
                    err = llm_chat_tools(sys_prompt, messages,
                        tools_json, &resp);
                    gettimeofday(&tv_end, NULL);
                    latency_ms = calc_elapsed_ms(&tv_start, &tv_end);

                    /* Watchdog check on cascade retry */
                    if (llm_call_timed_out(latency_ms)) {
                        syslog(LOG_WARNING,
                            "[%s] LLM watchdog: cascade retry "
                            "took %" PRIu32 " ms\n",
                            TAG, latency_ms);
                        free(fallback_text);
                        fallback_text = NULL;
                        {
                            char* short_text = voice_f2_timeout_short_text(msg,
                                "cascade watchdog");
                            final_text = (short_text != NULL)
                                ? short_text : strdup(LLM_TIMEOUT_MSG);
                        }
                        watchdog_fired = true;
                        agent_trace_step(&trace, iteration, NULL,
                            latency_ms, 0);
                        llm_response_free(&resp);
                        break;
                    }

                    if (err == OK && resp.text && resp.text_len > 0) {
                        final_text = strdup(resp.text);
                        free(fallback_text);
                        fallback_text = NULL;
                    } else {
                        /* Premium failed — fall back to cheap response */
                        final_text = fallback_text;
                        fallback_text = NULL;
                    }
                    agent_trace_step(&trace, iteration, NULL,
                        latency_ms, err == OK);
                    llm_response_free(&resp);
                    break;
                }
                /* No premium backend available — use cheap response */
                final_text = fallback_text;
                fallback_text = NULL;
                agent_trace_step(&trace, iteration, NULL, latency_ms, 1);
                break;
            }

            if (resp.text && resp.text_len > 0 && !final_text) {
                final_text = strdup(resp.text);
            }
#ifdef CONFIG_AI_AGENT_LVGL_UI
            /* 真正的最终回答：用完整文本收尾流式气泡（原地落历史/气泡，
             * SPEED-B 会对随后 outbound 镜像做一次性去重 → 历史只有一份）。
             * 用户看到的流式文字不会被清空再重建。 */
            if (stream_ctx.started) {
                lvgl_ui_channel_stream_end(resp.text ? resp.text : "", false);
                stream_ctx.started = false;
            }
            /* VOICE-B：最终回答收尾 —— flush 尾句并登记 takeover，随后
             * outbound dispatch 的整段 speak_async 会被哈希匹配跳过，
             * 因此不会重复播报已流式说过的句子。 */
            if (stream_ctx.voice_started) {
                voice_channel_stream_end(resp.text ? resp.text : "");
                stream_ctx.voice_started = false;
            }
#endif
            agent_trace_step(&trace, iteration, NULL, latency_ms, 1);
            llm_response_free(&resp);
            break;
        }

        syslog(LOG_INFO, "[%s] Tool iter %d: %d calls\n",
            TAG, iteration + 1, resp.call_count);

        /* Trace: log tool step */
        agent_trace_step(&trace, iteration,
            resp.call_count > 0 ? resp.calls[0].name : NULL,
            latency_ms, 1);

        /* Duplicate detection — break if stuck in a loop */
        bool should_break = check_tool_dup(
            &resp, prev_sig, &dup_count, prev_name, &name_repeat);

        if (should_break) {
            add_assistant_message(messages, &resp);
            add_tool_result_messages(messages, &resp, tool_output,
                tool_size, msg->channel, msg->chat_id);
            llm_response_free(&resp);
            break;
        }

        add_assistant_message(messages, &resp);
        add_tool_result_messages(messages, &resp, tool_output,
            tool_size, msg->channel, msg->chat_id);

        /* Local tool shortcut: if the single tool in this round is a
         * local file op, skip the next LLM round and use the tool
         * output directly as the reply. Saves ~2s.
         * Restricted to call_count == 1: the parallel path does not
         * write into tool_output, so multi-call rounds must go through
         * the LLM to aggregate results (and tool_output would be stale
         * from a prior iteration if we allowed call_count > 1 here). */
        if (resp.call_count == 1) {
            bool all_local = true;
            for (int i = 0; i < resp.call_count; i++) {
                if (strcmp(resp.calls[i].name, "read_file") != 0
                    && strcmp(resp.calls[i].name, "write_file") != 0
                    && strcmp(resp.calls[i].name, "edit_file") != 0
                    && strcmp(resp.calls[i].name, "list_dir") != 0) {
                    all_local = false;
                    break;
                }
            }
            if (all_local && tool_output[0]) {
                /* Don't shortcut if the tool read a skill file
                 * from AGENT_SKILLS_DIR — the LLM needs to process
                 * the skill content and generate a proper user-facing
                 * response instead of dumping raw markdown to the user. */
                bool is_skill_read = false;
                if (strcmp(resp.calls[0].name, "read_file") == 0
                    && resp.calls[0].input != NULL) {
                    cJSON *input_obj = cJSON_Parse(resp.calls[0].input);
                    if (input_obj) {
                        cJSON *path_obj = cJSON_GetObjectItem(input_obj, "path");
                        if (path_obj && cJSON_IsString(path_obj)
                            && strncmp(path_obj->valuestring,
                                AGENT_SKILLS_DIR,
                                strlen(AGENT_SKILLS_DIR)) == 0) {
                            is_skill_read = true;
                        }
                        cJSON_Delete(input_obj);
                    }
                }

                if (!is_skill_read) {
                    syslog(LOG_INFO,
                        "[%s] Local tool shortcut: skip LLM round\n",
                        TAG);
                    final_text = strdup(tool_output);
                    llm_response_free(&resp);
                    break;
                }

                syslog(LOG_INFO,
                    "[%s] Skill file read — no shortcut, LLM will process\n",
                    TAG);
            }
        }

        /* TASK_COMPLETE detection */
        if (resp.call_count == 1 && tool_output[0]
            && strstr(tool_output, "TASK_COMPLETE")) {
            syslog(LOG_INFO,
                "[%s] Tool %s returned TASK_COMPLETE\n",
                TAG, resp.calls[0].name);
            llm_response_free(&resp);
            bool task_timed_out = false;
            final_text = handle_task_complete(sys_prompt, messages,
                &task_timed_out);
            if (task_timed_out) {
                watchdog_fired = true;
            }
            break;
        }

        llm_response_free(&resp);
    }

    /* Iteration limit reached — force a summary reply */
    if (!final_text && iteration >= AGENT_AI_AGENT_MAX_TOOL_ITER) {
        final_text = force_finish_reply(sys_prompt, messages);
        agent_trace_end(&trace, AGENT_TRACE_TIMEOUT);
    } else if (!final_text && trace.total_tool_calls > 0) {
        /* Tools ran but the model closed with no text (observed with
         * StepFun right after a successful set_alarm). The action already
         * happened on the device, so confirm it instead of apologising. */
        final_text = strdup("好的，已经为你安排好了。");
        syslog(LOG_INFO,
            "[%s] Empty closing text after %d tool call(s); confirming\n",
            TAG, trace.total_tool_calls);
        agent_trace_end(&trace, AGENT_TRACE_OK);
    } else if (watchdog_fired) {
        agent_trace_end(&trace, AGENT_TRACE_TIMEOUT);
    } else if (final_text) {
        agent_trace_end(&trace, AGENT_TRACE_OK);
    } else {
        agent_trace_end(&trace, AGENT_TRACE_FAIL);
    }

send_reply:
    /* Cache store: save simple query responses for future reuse.
     * 键的构造与上面的 cache check 一致，必须带上人格。
     * LOCAL-FAST B-FIX（perf-fw 复核 P1）：本地回答（离线本地优先 / 断网
     * 端侧兜底，s_answer_from_local=true）不写 LLM cache —— 否则网络恢复后
     * 5 分钟内相同问题仍会命中低质量本地回答；时间类问题还会拿到陈旧时间。 */
    if (final_text && s_answer_from_local
        && complexity == LLM_COMPLEXITY_SIMPLE) {
        /* 验收打点：本地回答不写 cache（网络恢复后不会命中低质量本地答案） */
        syslog(LOG_INFO,
            "[%s] local-first: skip LLM cache store (local answer)\n", TAG);
    }
    if (final_text && msg->content && !s_answer_from_local
        && complexity == LLM_COMPLEXITY_SIMPLE) {
        char cache_key[LLM_CACHE_KEY_LEN + 8];

        build_llm_cache_key(cache_key, sizeof(cache_key), cache_persona,
            msg->content);
        llm_cache_put(cache_key, strlen(cache_key), final_text);
        if (last_total_tokens > 0) {
            llm_cache_put_tokens(cache_key, strlen(cache_key),
                last_total_tokens);
        }
    }

    return final_text;
}

/* ── Build messages array from session history + current msg ─ */

static cJSON* build_messages(const char* chat_id, const char* content,
    char* history_json, size_t hist_size)
{
    session_get_history_json(chat_id, history_json, hist_size,
        AGENT_AI_AGENT_MAX_HISTORY);

    cJSON* messages = cJSON_Parse(history_json);

    if (!messages) {
        messages = cJSON_CreateArray();
    }

    cJSON* user_msg = cJSON_CreateObject();

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddStringToObject(user_msg, "content", content);
    cJSON_AddItemToArray(messages, user_msg);
    return messages;
}

/* ── Inject session context into system prompt ───────────── */

static void inject_session_context(char* sys_prompt, size_t size,
    const char* channel, const char* chat_id)
{
    size_t len = strlen(sys_prompt);

    snprintf(sys_prompt + len, size - len,
        "\n## Current Session\n"
        "channel: %s\n"
        "chat_id: %s\n"
        "When using feishu_send_mention or feishu_chat_members, "
        "use the chat_id above unless the user specifies a "
        "different one.\n"
        "If the user message ends with "
        "[mentioned_users: name=open_id], "
        "use those open_ids when you need to @mention or remind "
        "those users. Do NOT include the [mentioned_users: ...] "
        "block in your reply text.\n",
        channel, chat_id);

    /* v114b5 / P0-NET：语音通道强制短答。P0-NET 起收紧为 1–3 句、≤80 字
     * （与 llm_proxy 语音 max_tokens 80 对齐；这段是固定串，利于前缀缓存命中）。 */
    if (strcmp(channel, AGENT_CHAN_VOICE) == 0) {
        size_t l2 = strlen(sys_prompt);
#if VOICE_BREVITY_PROMPT
        snprintf(sys_prompt + l2, size - l2,
            "\nIMPORTANT: This reply is spoken aloud on a watch. "
            "Answer in ONE TO THREE short sentences, at most 80 Chinese "
            "characters in total. Give the conclusion first; no lists, no "
            "markdown, no long explanations. If the full answer would be "
            "longer, summarize it to fit.\n");
#else
        snprintf(sys_prompt + l2, size - l2,
            "\nIMPORTANT: This reply is spoken aloud on a watch. "
            "Answer in ONE short paragraph of at most 90 Chinese characters. "
            "Be complete but concise; no lists, no markdown. If the full answer "
            "would be longer, summarize it to fit.\n");
#endif
    }
}

/* ── Main agent loop task ─────────────────────────────────── */

static void* agent_loop_task(void* arg)
{
    (void)arg;
    agent_mem_status_t mem_st;

    agent_mem_get_status(&mem_st);
    syslog(LOG_INFO, "[%s] Agent loop started, free heap: %zu\n",
        TAG, mem_st.free_heap);

    size_t ctx_size = agent_mem_safe_size(
        AGENT_CONTEXT_BUF_SIZE, 4 * 1024);
    size_t hist_size = agent_mem_safe_size(
        AGENT_LLM_STREAM_BUF_SIZE, 8 * 1024);
    size_t tool_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE, TOOL_OUTPUT_SIZE_MIN);

    char* sys_prompt = calloc(1, ctx_size);
    char* history_json = calloc(1, hist_size);
    char* tool_output = calloc(1, tool_size);

    if (!sys_prompt || !history_json || !tool_output) {
        syslog(LOG_ERR, "[%s] Failed to allocate agent buffers\n", TAG);
        free(sys_prompt);
        free(history_json);
        free(tool_output);
        return NULL;
    }

    syslog(LOG_INFO, "[%s] Buffers: ctx=%zu hist=%zu tool=%zu\n",
        TAG, ctx_size, hist_size, tool_size);

    /* Initialize memory pool for parallel tool outputs */
    size_t pool_buf_size = agent_mem_safe_size(
        TOOL_OUTPUT_SIZE_LARGE, TOOL_OUTPUT_SIZE_MIN);
    if (agent_pool_init(&s_tool_pool, pool_buf_size,
            AGENT_MAX_TOOL_CALLS)
        == OK) {
        s_pool_ready = true;
        syslog(LOG_INFO, "[%s] Tool output pool: %d x %zu bytes\n",
            TAG, AGENT_MAX_TOOL_CALLS, pool_buf_size);
    } else {
        syslog(LOG_WARNING,
            "[%s] Pool init failed, using heap fallback\n", TAG);
    }

    char* tools_json = tool_registry_get_tools_json();

    syslog(LOG_INFO, "[%s] Tools JSON loaded: %d bytes\n",
        TAG, tools_json ? (int)strlen(tools_json) : 0);

    /* P0-NET：网络就绪时做一次保守预连接（只 TCP+TLS 握手，不发请求）。
     * 不新增线程：复用本 agent 线程，在"还没有消息"的等待窗口里做；
     * 成功后恢复无限等待；失败最多重试约 30s 后放弃（静默，不影响回合）。 */
#if P0_NET_PRECONNECT
    bool preconnect_done = false;
    int preconnect_tries = 0;
    int offline_polls = 0;   /* 无网时最多轮询 ~3 分钟，之后恢复无限等待 */
#endif

    while (!agent_shutdown_requested()) {
        agent_msg_t msg;
        uint32_t pop_wait = UINT32_MAX;

#if P0_NET_PRECONNECT
        /* P0-NET（perf-fw 复核 #2）：断链/池失效后重置 preconnect_done，
         * 下一次网络恢复时重建暖连接（vela_tls_pool_invalidate 也由网络
         * 断链触发，池中连接会在下一次请求 acquire 时丢弃）。 */
        if (network_get_state() != NET_STATE_CONNECTED && preconnect_done) {
            preconnect_done = false;
            preconnect_tries = 0;
            offline_polls = 0;
            syslog(LOG_INFO,
                "[%s] [lat] preconnect reset (link down)\n", TAG);
        }
        if (!preconnect_done)
            pop_wait = 1500;   /* 网络就绪前用短超时轮询，顺便做预连接 */
#endif
        int err = message_bus_pop_inbound(&msg, pop_wait);

        if (err != OK) {
#if P0_NET_PRECONNECT
            if (pop_wait != UINT32_MAX) {
                if (network_get_state() != NET_STATE_CONNECTED) {
                    /* 还没网：最多轮询 ~3 分钟（120×1.5s），之后不再为预连接
                     * 唤醒；真到有网时首个请求走原有"连接+握手"路径。 */
                    if (++offline_polls >= 120) {
                        preconnect_done = true;
                        syslog(LOG_INFO,
                            "[%s] [lat] preconnect: still offline after %ds, "
                            "give up\n", TAG, offline_polls * 1500 / 1000);
                    }
                } else {
                    preconnect_tries++;
                    int pr = llm_proxy_preconnect();

                    syslog(LOG_INFO,
                        "[%s] [lat] preconnect attempt=%d result=%d\n",
                        TAG, preconnect_tries, pr);
                    if (pr == OK || preconnect_tries >= 20) {
                        if (pr != OK) {
                            syslog(LOG_WARNING,
                                "[%s] [lat] preconnect give up after %d tries\n",
                                TAG, preconnect_tries);
                        }
                        preconnect_done = true;
                    }
                }
            }
#endif
            continue;
        }

        if (agent_shutdown_requested()) {
            free(msg.content);
            free(msg.image_b64);
            break;
        }

        /* ★ 2026-11-01 v102c：串行化"用户回合"。
         * 真机复现：上一轮回答的 TTS 还在拉流（mimo POST / TLS）时，下一条
         * 消息立刻发起第二个 LLM 请求，两条 TLS 流并发把 BT PAN 的 zblue
         * 缓冲打爆 → 整机冻死（无断言、看门狗复位）。
         *
         * ★ 2026-11-03 v125 P0-B：只有 voice 通道才需要"等上一轮播报结束"。
         * 非语音通道（ble/cli/ws/lvgl_ui/...）不再被上一轮 TTS 播报阻塞：
         * perf-net 实测 ask2 因本门禁多等 25 s、ask3 多等 46 s
         * （serial_perf_session.log:1315/1640，报告 §2.4）。
         *
         * 为什么跳过不产生并发 TLS 风险：HTTPS POST 已经有**全局串行锁**
         * vela_https_post_json()（infra/vela_tls.c，LLM 与 MiMo TTS 都走它），
         * TTS 侧另有 voice_channel_llm_begin() 媒体回合互斥；两条 POST 不会
         * 并发。非语音通道最多等 500 ms（仅为避开正在 close/drain 的播放器，
         * 不抢音频设备），超时立即处理。 */
        struct timeval turn_gate_t0;
        gettimeofday(&turn_gate_t0, NULL);

        if (voice_channel_is_busy())
          {
            int waited;
            /* v127（2026-11-03 稳定性优先回滚）：非语音通道恢复等待上一轮 TTS。
             * v125 为提速把非语音上限压到 500ms，但现场出现播放期间新 LLM 请求
             * 与 TTS 网络并发 → BT PAN 小缓冲压力/看门狗风险（v102c 同类问题）。
             * 速度优化改用后续「流式 LLM + 可中止 TTS」方案，不再靠放开互斥。 */
            int is_voice = (strcmp(msg.channel, AGENT_CHAN_VOICE) == 0);
            int max_wait_ms = is_voice ? 30000 : 300;

            if (!is_voice)
              {
                /* v130（2026-11-03）：非语音新消息优先。直接打断上一句 TTS，
                 * 不再傻等 30s —— 既提升 App 聊天响应，也避免“旧 TTS 网络 +
                 * 新 LLM POST”并发（v102c 曾因此看门狗复位）。 */
                voice_channel_tts_abort();
                syslog(LOG_INFO,
                   "[%s] previous TTS busy -> aborted for non-voice msg (chan=%s)\n",
                   TAG, msg.channel);
              }
            else
              {
                syslog(LOG_INFO,
                   "[%s] previous TTS still busy: waiting up to %dms (chan=%s)\n",
                   TAG, max_wait_ms, msg.channel);
              }

            for (waited = 0; waited < max_wait_ms
                && voice_channel_is_busy(); waited += 100)
              {
                usleep(100 * 1000);
              }
            syslog(LOG_INFO,
                   "[%s] turn gate: waited %dms (chan=%s)\n",
                   TAG, waited, msg.channel);
          }

        /* v125 P0-B：per-turn 计时起点。下面打印的所有毫秒差都相对**本轮**
         * 起点重新算，不累加历史（旧 [voice] "LLM latency" 打的是陈旧累计值，
         * 见 perf-net §2.5，已在 voice_channel.c 修掉）。 */
        struct timeval turn_t0;
        gettimeofday(&turn_t0, NULL);
        long gate_ms = (long)calc_elapsed_ms(&turn_gate_t0, &turn_t0);
#if P0_NET_LAT
        /* P0-NET：本轮起点记录建连序号；结束时若序号变化才算“本轮新建连接”，
         * 否则一律标记 cache（conn/hs 记 0），避免把上一轮/预连接的耗时算进来。 */
        uint64_t lat_conn_seq0 = vela_tls_conn_seq();
#endif
        syslog(LOG_INFO, "[%s] [turn] chan=%s begin gate_ms=%ld\n",
            TAG, msg.channel, gate_ms);

        syslog(LOG_INFO, "[%s] Processing message from %s:%s\n",
            TAG, msg.channel, msg.chat_id);

        /* Check memory pressure */
        agent_mem_get_status(&mem_st);
        if (mem_st.free_heap < AGENT_MEM_RESERVE_BYTES) {
            syslog(LOG_WARNING,
                "[%s] Low memory: %zu bytes free, skipping\n",
                TAG, mem_st.free_heap);
            agent_msg_t out = { 0 };
            strncpy(out.channel, msg.channel,
                sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id,
                sizeof(out.chat_id) - 1);
            out.content = strdup("系统内存不足，请稍后再试。");
            /* 低内存提示不是回答：只说给屏幕听 (P2a)。 */
            out.kind = AGENT_MSG_KIND_ERROR;
            if (out.content) {
                if (message_bus_push_outbound(&out) != OK) {
                    free(out.content);
                }
            }
            struct timeval lowmem_t;
            gettimeofday(&lowmem_t, NULL);
            syslog(LOG_INFO,
                "[%s] [turn] chan=%s low_memory total_ms=%ld\n",
                TAG, msg.channel,
                (long)calc_elapsed_ms(&turn_t0, &lowmem_t));
            free(msg.content);
            continue;
        }

        /* Slash commands — fast path, bypass LLM */
        char* reply = handle_slash_command(&msg);

        /* Prompt injection check — before LLM */
        if (!reply && tool_guard_check_injection(msg.content)) {
            syslog(LOG_WARNING,
                "[%s] Prompt injection blocked from %s:%s\n",
                TAG, msg.channel, msg.chat_id);
            reply = strdup("I can't do that.");
            if (!reply) {
                /* Fail-closed: skip this message entirely */
                free(msg.content);
                continue;
            }
        }

        /* DEMO-FW：固定短语本地极速回复，先于（且独立于）NL 规则表；
         * 开关见文件顶部 DEMO_FAST_REPLY。 */
#if DEMO_FAST_REPLY
        if (!reply) {
            reply = handle_demo_fast_path(msg.channel, msg.chat_id,
                msg.content);
        }
#endif

        /* NL fast path — after injection check, before LLM */
        if (!reply) {
            reply = handle_nl_fast_path(msg.content);
        }

        if (reply) {
            agent_msg_t out = { 0 };
            strncpy(out.channel, msg.channel,
                sizeof(out.channel) - 1);
            strncpy(out.chat_id, msg.chat_id,
                sizeof(out.chat_id) - 1);
            out.content = reply;
            /* Slash commands and the NL fast path (时间/电量/心率/步数/音乐,
             * 技能列表, 天气) are handled on the device by tools - no model is
             * involved, so label them 本地 rather than letting them inherit
             * the default 云端. */
            out.from_local = true;
            /* 端侧快通道的答复同样是要念给用户听的最终回答。 */
            out.kind = AGENT_MSG_KIND_FINAL;
            if (message_bus_push_outbound(&out) != OK) {
                free(reply);
            }
            /* v125 P0-B per-turn：端侧快通道（slash/时间/电量/天气…）同样
             * 打印本轮耗时，单轮差值、不累加。 */
            struct timeval fast_t;
            gettimeofday(&fast_t, NULL);
            syslog(LOG_INFO,
                "[%s] [turn] chan=%s fast_path llm_ms=0 total_ms=%ld\n",
                TAG, msg.channel,
                (long)calc_elapsed_ms(&turn_t0, &fast_t));
            free(msg.content);
            continue;
        }

        /* Hot-reload skills if directory changed */
        if (skill_loader_check_changed()) {
            syslog(LOG_INFO, "[%s] Skills changed, refreshing\n", TAG);
            skill_loader_refresh();
        }

        syslog(LOG_INFO, "[%s] ctx probe: begin (chan=%s)\n", TAG, msg.channel);
        context_build_system_prompt(sys_prompt, ctx_size);
        syslog(LOG_INFO, "[%s] ctx probe: after_ctx (sys_len=%d)\n",
               TAG, (int)strlen(sys_prompt));
        inject_session_context(sys_prompt, ctx_size,
            msg.channel, msg.chat_id);
        syslog(LOG_INFO, "[%s] ctx probe: after_inject (sys_len=%d)\n",
               TAG, (int)strlen(sys_prompt));

        /* Refresh tools JSON — Node tools are dynamic */
        free(tools_json);
        tools_json = tool_registry_get_tools_json();
        syslog(LOG_INFO, "[%s] ctx probe: after_tools_json (len=%d)\n",
               TAG, tools_json ? (int)strlen(tools_json) : -1);

        cJSON* messages = build_messages(msg.chat_id, msg.content,
            history_json, hist_size);
        syslog(LOG_INFO, "[%s] ctx probe: after_messages (ok=%d)\n",
               TAG, messages != NULL);

        char* final_text = NULL;

        /* Vision path */
        final_text = handle_vision_message(&msg);
        if (final_text) {
            cJSON_Delete(messages);
        } else {
            /* v114b10：语音通道限制回答长度（≈70–90 汉字，完整可播且更快） */
            llm_proxy_set_max_tokens(
                strcmp(msg.channel, AGENT_CHAN_VOICE) == 0
                    ? AGENT_LLM_MAX_TOKENS_VOICE : 0);

            /* ReAct loop */
            s_llm_path_count++;
            final_text = run_react_loop(sys_prompt, messages,
                tools_json, tool_output, tool_size, &msg);
            syslog(LOG_INFO, "[%s] probe: react_return (txt=%d)\n",
                   TAG, final_text ? (int)strlen(final_text) : -1);
            cJSON_Delete(messages);
            syslog(LOG_INFO, "[%s] probe: messages_freed\n", TAG);
        }

        /* v125 P0-B per-turn：LLM 完成（含 ReAct 多轮）与派发耗时的单轮差值。
         * TTS 起始/结束的差值由 voice_channel.c 在自己的 speak 时间轴里打印
         * （TTS start after ASR / first chunk / network done / speak done）。 */
        struct timeval llm_done_t;
        gettimeofday(&llm_done_t, NULL);
        long llm_ms = (long)calc_elapsed_ms(&turn_t0, &llm_done_t);
        syslog(LOG_INFO,
            "[%s] [turn] chan=%s llm_done llm_ms=%ld first_token_ms=%ld stream_deltas=%d\n",
            TAG, msg.channel, llm_ms, s_turn_stream_first_ms,
            s_turn_stream_deltas);

        /* P0-NET：LLM 阶段结束——按建连序号判断本轮是否真的新建了连接：
         * 序号变化且 pool_hit=false → conn/hs 取本轮值；否则本轮回合是 cache
         * （复用池连接/预连接），conn/hs 记 0。后面的 TTS 网络会覆盖 vela_tls
         * 的 last timing，所以必须此时取。 */
#if P0_NET_LAT
        int64_t lat_conn_ms = 0;
        int64_t lat_hs_ms = 0;
        bool lat_new_conn = false;
        if (vela_tls_conn_seq() != lat_conn_seq0) {
            bool hit = true;

            vela_tls_get_last_timing(&lat_conn_ms, &lat_hs_ms, &hit);
            if (hit) {
                lat_conn_ms = 0;
                lat_hs_ms = 0;
            } else {
                lat_new_conn = true;
            }
        }
        /* TTS 首块序号：dispatch 前抓一次，dispatch 后变化才算本轮 tts_first。 */
        unsigned lat_tts_seq0 = 0;
        voice_channel_get_last_tts_first_ms(NULL, &lat_tts_seq0);
#endif

        syslog(LOG_INFO, "[%s] probe: dispatch_begin\n", TAG);
        dispatch_response(&msg, final_text);
        syslog(LOG_INFO, "[%s] probe: dispatch_end\n", TAG);

#if P0_NET_LAT
        /* P0-NET：本轮 TTS 首块延迟（voice_channel 的时间轴，单位 ms，-1=本轮
         * 没走 TTS/没收到首块）。序号变化才取，避免拿到上一轮的旧值。 */
        long lat_tts_first_ms = -1;
        unsigned lat_tts_seq1 = 0;

        voice_channel_get_last_tts_first_ms(&lat_tts_first_ms, &lat_tts_seq1);
        if (lat_tts_seq1 == lat_tts_seq0)
            lat_tts_first_ms = -1;
        else if (lat_tts_first_ms < 0)
            lat_tts_first_ms = -1;
#endif

        struct timeval turn_end_t;
        gettimeofday(&turn_end_t, NULL);
        long dispatch_ms = (long)calc_elapsed_ms(&llm_done_t, &turn_end_t);
        long total_ms = gate_ms + llm_ms + dispatch_ms;

        syslog(LOG_INFO,
            "[%s] [turn] chan=%s done llm_ms=%ld dispatch_ms=%ld total_ms=%ld\n",
            TAG, msg.channel, llm_ms, dispatch_ms, total_ms);

#if P0_NET_LAT
        /* P0-NET：单轮 [lat] 聚合（分段口径，全部为**本轮局部值**）。
         * conn/new|cache：conn_new 表示本轮真的建连（conn/hs 有效）；cache 表示
         * 复用池连接/预连接（conn/hs 记 0）。
         * tts_first_ms：来自 voice_channel 的 TTS 首块时间轴（相对 TTS 开始），
         * 本轮序号变化才取，否则 -1（本轮没走 TTS/没收到首块）。 */
        {
            long gen_ms = (s_turn_stream_first_ms > 0
                    && llm_ms >= s_turn_stream_first_ms)
                ? llm_ms - s_turn_stream_first_ms : -1;

            syslog(LOG_INFO,
                "[%s] [lat] turn chan=%s conn=%s conn_ms=%lld "
                "handshake_ms=%lld pool=%d ttft_ms=%ld gen_ms=%ld "
                "tts_first_ms=%ld llm_ms=%ld dispatch_ms=%ld total_ms=%ld\n",
                TAG, msg.channel, lat_new_conn ? "new" : "cache",
                (long long)lat_conn_ms, (long long)lat_hs_ms,
                lat_new_conn ? 0 : 1, s_turn_stream_first_ms, gen_ms,
                lat_tts_first_ms, llm_ms, dispatch_ms, total_ms);
        }
#endif

        /* Free image_b64 if not already freed by vision path */
        free(msg.image_b64);
        msg.image_b64 = NULL;
        free(msg.content);
    }

    /* Cleanup (unreachable in normal operation) */
    if (s_pool_ready) {
        agent_pool_destroy(&s_tool_pool);
        s_pool_ready = false;
    }
    free(tools_json);
    free(sys_prompt);
    free(history_json);
    free(tool_output);
    return NULL;
}

/* ── Public interface ─────────────────────────────────────── */

int agent_loop_init(void)
{
    syslog(LOG_INFO, "[%s] Agent loop initialized\n", TAG);

#if LOCAL_FIRST_WARMUP
    /* LOCAL-FAST B（docs/108 §2.B）：在 ai_agent 主进程上下文预热端侧模型，
     * 让"首次 fallback/弱网本地回答"不再付 ai_lm_init()+AllocateTensors 的
     * 一次性时间。A 内部是一次性 detached 8KB 线程，失败自动忽略；这里
     * 不等待、不新增常驻线程。必须在主进程上下文调用（不要挪到 NSH 短任务组，
     * 否则线程会被任务组清理掉 —— demo_voice 踩过同类坑）。 */
    local_lm_warmup_start();
    syslog(LOG_INFO, "[%s] local-first: warmup requested\n", TAG);
#endif

    return OK;
}

int agent_loop_start(void)
{
    static bool s_started = false;
    int ret;

    /* Called from three places now: boot phase 5, and the two network-up
     * paths that predate it.  The loop is the only consumer of the inbound
     * queue, so it has to run even when the link never comes up -- that is
     * what makes the on-device model reachable offline.  Guard here rather
     * than at the call sites so none of them can start a second thread.
     */

    if (s_started) {
        return OK;
    }

    ret = agent_task_create(agent_loop_task, "agent_loop",
        AGENT_AI_AGENT_STACK, NULL, AGENT_AI_AGENT_PRIO);

    if (ret != OK) {
        syslog(LOG_ERR,
            "[%s] Failed to create agent_loop task\n", TAG);
        return ret;
    }

    s_started = true;
    return OK;
}
