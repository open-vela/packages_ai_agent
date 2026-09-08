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
#include "core/session_mgr.h"
#include "llm/llm_cache.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "tools/skill_loader.h"
#ifdef CONFIG_AI_AGENT_LOCAL_TOOL_ROUTER
#include "tools/mooncat_local_router.h"
#endif
#include "tools/tool_guard.h"
#include "tools/tool_registry.h"
#include "agent_compat.h"
#include "agent_config.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include "cJSON.h"

static const char* TAG = "agent";

#define TOOL_OUTPUT_SIZE (8 * 1024)
#define TOOL_OUTPUT_SIZE_LARGE (16 * 1024)
#define TOOL_OUTPUT_SIZE_MIN (2 * 1024)

/* ── Forward declarations ──────────────────────────────────── */

static char* handle_slash_note(const agent_msg_t* msg);
static char* handle_slash_remind(const agent_msg_t* msg);
static bool llm_call_timed_out(uint32_t latency_ms);

/* ── Timeout message constants ─────────────────────────────── */

#define LLM_TIMEOUT_MSG \
    "请求超时，LLM 响应时间过长。请稍后重试，或尝试简化你的问题。"
#define LLM_TIMEOUT_TASK_COMPLETE_MSG \
    "任务已完成，但生成确认消息超时。"

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

static const char* kw_time[]       = { "几点了", "什么时间", "what time", "几点钟", NULL };
static const char* kw_battery[]    = { "电量多少", "电池电量", "battery", NULL };
static const char* kw_heartrate[]  = { "心率多少", "heart rate", "心率是多", NULL };
static const char* kw_steps[]      = { "走了多少步", "今天走了多少", "steps", "步数", NULL };
static const char* kw_pause[]      = { "暂停音乐", "暂停播放", "pause music", NULL };
static const char* kw_stop[]       = { "停止播放", "停止音乐", "stop music", NULL };
static const char* kw_resume[]     = { "继续播放", "resume", NULL };

static const nl_intent_t s_intents[] = {
    { kw_time,      "get_current_time", "{}",  256  },
    { kw_battery,   "get_battery",      "{}",  512  },
    { kw_heartrate, "get_heartrate",    "{}",  256  },
    { kw_steps,     "get_steps",        "{}",  256  },
    { kw_pause,     "music_pause",      "{}",  256  },
    { kw_stop,      "music_stop",       "{}",  256  },
    { kw_resume,    "music_resume",     "{}",  256  },
    { NULL,         NULL,               NULL,  0    },
};

static char* handle_nl_fast_path(const char* text)
{
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

        const char* p = strstr(text, "天气");
        if (p && p > text && (size_t)(p - text) < sizeof(safe_city) - 1) {
            int ci = 0;
            for (const char* s = text; s < p && ci < 62; s++) {
                if (*s != '"' && *s != '\\' && *s != ' ')
                    safe_city[ci++] = *s;
            }
            if (ci > 0) safe_city[ci] = '\0';
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

#ifdef CONFIG_AI_AGENT_LOCAL_TOOL_ROUTER
    /* Preserve all established keyword routes; NULL continues to cloud LLM. */
    return mooncat_local_router_handle(text);
#else
    return NULL;
#endif
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

static void dispatch_response(const agent_msg_t* msg,
    char* final_text)
{
    if (final_text && final_text[0]) {
        session_append(msg->chat_id, "user", msg->content);
        session_append(msg->chat_id, "assistant", final_text);

        agent_msg_t out = { 0 };
        strncpy(out.channel, msg->channel,
            sizeof(out.channel) - 1);
        strncpy(out.chat_id, msg->chat_id,
            sizeof(out.chat_id) - 1);
        out.content = final_text;
        if (message_bus_push_outbound(&out) != OK) {
            free(final_text);
        }
    } else {
        free(final_text);
        agent_msg_t out = { 0 };
        strncpy(out.channel, msg->channel,
            sizeof(out.channel) - 1);
        strncpy(out.chat_id, msg->chat_id,
            sizeof(out.chat_id) - 1);
        out.content = strdup("Sorry, I encountered an error.");
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
 * (TTS synthesis of a status phrase wastes time and memory). */
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

    prev_sig[0] = '\0';
    dup_count = 0;
    prev_name[0] = '\0';
    name_repeat = 0;
    int last_total_tokens = 0;
    bool watchdog_fired = false;

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

    /* Cache check: for simple queries, try cache before LLM call */
    if (msg->content && complexity == LLM_COMPLEXITY_SIMPLE) {
        char* cached = llm_cache_get(msg->content, strlen(msg->content));
        if (cached) {
            syslog(LOG_INFO, "[%s] Cache hit, skipping LLM call\n", TAG);
            final_text = cached;
            agent_trace_step(&trace, 0, NULL, 0, 1);
            agent_trace_end(&trace, AGENT_TRACE_OK);
            goto send_reply;
        }
    }

    for (iteration = 0; iteration < AGENT_AI_AGENT_MAX_TOOL_ITER;
         iteration++) {
        send_working_status(msg, iteration);

        llm_response_t resp;
        struct timeval tv_start, tv_end;
        gettimeofday(&tv_start, NULL);
        int err = llm_chat_tools(sys_prompt, messages, tools_json, &resp);
        gettimeofday(&tv_end, NULL);
        uint32_t latency_ms = calc_elapsed_ms(&tv_start, &tv_end);

        /* Router failover: on LLM call failure, try next backend */
        if (err != OK && router_idx >= 0) {
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
            /* Distinguish timeout-induced failure from other errors */
            if (llm_call_timed_out(latency_ms)) {
                syslog(LOG_WARNING,
                    "[%s] LLM watchdog: call failed after %" PRIu32 " ms "
                    "(limit %ds)\n",
                    TAG, latency_ms, AGENT_LLM_TIMEOUT_SEC);
                agent_trace_step(&trace, iteration, NULL,
                    latency_ms, 0);
                llm_response_free(&resp);
                final_text = strdup(LLM_TIMEOUT_MSG);
                watchdog_fired = true;
                break;
            }
            syslog(LOG_ERR, "[%s] LLM call failed (iter %d)\n",
                TAG, iteration);
            agent_trace_step(&trace, iteration, NULL, latency_ms, 0);
            llm_response_free(&resp);
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
            final_text = strdup(LLM_TIMEOUT_MSG);
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
                        final_text = strdup(LLM_TIMEOUT_MSG);
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
    } else if (watchdog_fired) {
        agent_trace_end(&trace, AGENT_TRACE_TIMEOUT);
    } else if (final_text) {
        agent_trace_end(&trace, AGENT_TRACE_OK);
    } else {
        agent_trace_end(&trace, AGENT_TRACE_FAIL);
    }

send_reply:
    /* Cache store: save simple query responses for future reuse */
    if (final_text && msg->content
        && complexity == LLM_COMPLEXITY_SIMPLE) {
        llm_cache_put(msg->content, strlen(msg->content), final_text);
        if (last_total_tokens > 0) {
            llm_cache_put_tokens(msg->content, strlen(msg->content),
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

    while (!agent_shutdown_requested()) {
        agent_msg_t msg;
        int err = message_bus_pop_inbound(&msg, UINT32_MAX);

        if (err != OK) {
            continue;
        }

        if (agent_shutdown_requested()) {
            free(msg.content);
            free(msg.image_b64);
            break;
        }

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
            if (out.content) {
                if (message_bus_push_outbound(&out) != OK) {
                    free(out.content);
                }
            }
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
            if (message_bus_push_outbound(&out) != OK) {
                free(reply);
            }
            free(msg.content);
            continue;
        }

        /* Hot-reload skills if directory changed */
        if (skill_loader_check_changed()) {
            syslog(LOG_INFO, "[%s] Skills changed, refreshing\n", TAG);
            skill_loader_refresh();
        }

        context_build_system_prompt(sys_prompt, ctx_size);
        inject_session_context(sys_prompt, ctx_size,
            msg.channel, msg.chat_id);

        /* Refresh tools JSON — Node tools are dynamic */
        free(tools_json);
        tools_json = tool_registry_get_tools_json();

        cJSON* messages = build_messages(msg.chat_id, msg.content,
            history_json, hist_size);

        char* final_text = NULL;

        /* Vision path */
        final_text = handle_vision_message(&msg);
        if (final_text) {
            cJSON_Delete(messages);
        } else {
            /* ReAct loop */
            s_llm_path_count++;
            final_text = run_react_loop(sys_prompt, messages,
                tools_json, tool_output, tool_size, &msg);
            cJSON_Delete(messages);
        }

        dispatch_response(&msg, final_text);

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
    return OK;
}

int agent_loop_start(void)
{
    int ret = agent_task_create(agent_loop_task, "agent_loop",
        AGENT_AI_AGENT_STACK, NULL, AGENT_AI_AGENT_PRIO);

    if (ret != OK) {
        syslog(LOG_ERR,
            "[%s] Failed to create agent_loop task\n", TAG);
    }
    return ret;
}
