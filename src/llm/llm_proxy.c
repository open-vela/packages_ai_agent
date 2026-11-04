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

#include "llm/llm_internal.h"
#include "llm/llm_parse.h"
#include "llm/llm_proxy.h"
#include "core/message_bus.h"
#include "voice/voice_channel.h" /* voice_channel_llm_begin/end（LLM↔TTS 互斥） */
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"
#include "agent_config.h"

/* P0-NET：MiMo token-plan 网关同时承载 LLM/TTS/ASR（见 mimo_key_local.h 注释），
 * 预连接该 host 可同时暖三条链路。头文件可能不存在（干净 checkout）→ 退回默认。 */
#if defined(__has_include)
#  if __has_include("voice/mimo_key_local.h")
#    include "voice/mimo_key_local.h"
#  endif
#endif
#ifndef MIMO_API_HOST
#  define MIMO_API_HOST "token-plan-cn.xiaomimimo.com"
#  define MIMO_API_PORT "443"
#endif

#ifdef CONFIG_AI_AGENT_NET_RPMSG
#include "network/network_manager.h"
#endif

#include "cJSON.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char* TAG = "llm";

static char s_api_key[128] = { 0 };
static char s_model[64] = AGENT_LLM_DEFAULT_MODEL;
static char s_llm_host[128] = AGENT_LLM_API_HOST;
static char s_llm_path[128] = AGENT_LLM_API_PATH;
static char s_llm_port[8] = "443"; /* "443" for HTTPS, "80" etc for HTTP */

/* Independent vision model config — empty means fall back to main LLM */
static char s_vision_model[64] = { 0 };
static char s_vision_host[128] = { 0 };
static char s_vision_api_key[128] = { 0 };

static pthread_mutex_t s_llm_lock = PTHREAD_MUTEX_INITIALIZER;

/* Check if host uses OpenAI-compatible max_completion_tokens param */
bool is_openai_compat_host(const char* host)
{
    return strstr(host, "openai.com")
        || strstr(host, "openrouter.ai")
        || strstr(host, "xiaomimimo.com");
}

/* Xiaomi MiMo 默认会先"深度思考"再回答: 手表语音场景优先低延迟, 显式关掉
 * (API 文档: extra_body {"thinking": {"type": "disabled"}}; 实测关闭后
 *  reasoning_tokens=0, 时延明显下降)。 */
static bool host_is_mimo(const char* host)
{
    return host != NULL && strstr(host, "xiaomimimo") != NULL;
}

static void add_mimo_thinking_disabled(cJSON* body)
{
    cJSON* thinking = cJSON_CreateObject();

    syslog(LOG_INFO, "[%s] MiMo thinking=disabled injected\n", "llm");

    if (thinking != NULL) {
        cJSON_AddStringToObject(thinking, "type", "disabled");
        cJSON_AddItemToObject(body, "thinking", thinking);
    }
}


int resp_buf_init(resp_buf_t* rb, size_t initial_cap)
{
    rb->data = calloc(1, initial_cap);
    if (!rb->data)
        return ERROR;
    rb->len = 0;
    rb->cap = initial_cap;
    return OK;
}

int resp_buf_append(resp_buf_t* rb, const char* data, size_t len)
{
    while (rb->len + len >= rb->cap) {
        size_t new_cap = rb->cap * 2;
        if (new_cap > AGENT_LLM_MAX_RESP_SIZE) {
            syslog(LOG_ERR, "llm: resp_buf exceeded %d limit\n",
                AGENT_LLM_MAX_RESP_SIZE);
            return ERROR;
        }
        char* tmp = realloc(rb->data, new_cap);
        if (!tmp)
            return ERROR;
        rb->data = tmp;
        rb->cap = new_cap;
    }
    memcpy(rb->data + rb->len, data, len);
    rb->len += len;
    rb->data[rb->len] = '\0';
    return OK;
}

void resp_buf_free(resp_buf_t* rb)
{
    if (rb->data) {
        free(rb->data);
        rb->data = NULL;
    }
    rb->len = 0;
    rb->cap = 0;
}

/* ── Init ─────────────────────────────────────────────────── */

int llm_proxy_init(void)
{
    if (AGENT_SECRET_API_KEY[0])
        strncpy(s_api_key, AGENT_SECRET_API_KEY, sizeof(s_api_key) - 1);
    if (AGENT_SECRET_MODEL[0])
        strncpy(s_model, AGENT_SECRET_MODEL, sizeof(s_model) - 1);

    char tmp[128] = { 0 };
    if (claw_config_get(AGENT_CFG_KEY_API_KEY, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_api_key, tmp, sizeof(s_api_key) - 1);
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_MODEL, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_model, tmp, sizeof(s_model) - 1);

    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_LLM_HOST, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_llm_host, tmp, sizeof(s_llm_host) - 1);
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_LLM_PATH, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_llm_path, tmp, sizeof(s_llm_path) - 1);
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get("llm_port", tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_llm_port, tmp, sizeof(s_llm_port) - 1);

    /* Load optional vision model config — empty falls back to main LLM */
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_VISION_MODEL, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_vision_model, tmp, sizeof(s_vision_model) - 1);
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_VISION_HOST, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_vision_host, tmp, sizeof(s_vision_host) - 1);
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_VISION_API_KEY, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_vision_api_key, tmp, sizeof(s_vision_api_key) - 1);

    if (s_api_key[0])
        syslog(LOG_INFO, "[%s] LLM proxy initialized (model: %s, host: %s)\n", TAG,
            s_model, s_llm_host);
    else
        syslog(LOG_WARNING, "[%s] No API key. Use CLI: set_llm <preset> <key>\n", TAG);
    return OK;
}

int llm_set_backend(const char* host, const char* path)
{
    pthread_mutex_lock(&s_llm_lock);
    if (host && host[0]) {
        claw_config_set(AGENT_CFG_KEY_LLM_HOST, host);
        strncpy(s_llm_host, host, sizeof(s_llm_host) - 1);
    }
    if (path && path[0]) {
        claw_config_set(AGENT_CFG_KEY_LLM_PATH, path);
        strncpy(s_llm_path, path, sizeof(s_llm_path) - 1);
    }
    pthread_mutex_unlock(&s_llm_lock);
    syslog(LOG_INFO, "[%s] LLM backend: %s%s\n", TAG, s_llm_host, s_llm_path);
    return OK;
}

int llm_set_port(const char* port)
{
    if (port && port[0]) {
        claw_config_set("llm_port", port);
        strncpy(s_llm_port, port, sizeof(s_llm_port) - 1);
        s_llm_port[sizeof(s_llm_port) - 1] = '\0';
    }
    return OK;
}

/* v114b10：按通道覆盖 max_tokens（0=默认） */
static int s_max_tokens_override;

/* P0-NET（2026-11-04；perf-fw 复核修正）：语音 max_tokens 默认**不降**，保持
 * 110。原因：80 会截断工具轮/中文长回答，工具调用参数也可能被截断。
 * 语音短答只靠 prompt 约束（1–3 句、≤80 字，见 agent_loop.c 的 voice prompt）。
 * 如仍想实验更短回答，可显式 -DLLM_VOICE_MAX_TOKENS=80（可编译开关）。 */
#ifndef LLM_VOICE_MAX_TOKENS
#define LLM_VOICE_MAX_TOKENS 0
#endif

/* 把"通道标记 110"换算成实际发送值；其它覆盖值/默认值原样返回。 */
static int llm_effective_max_tokens(int default_tokens)
{
    int v = s_max_tokens_override > 0 ? s_max_tokens_override
                                      : default_tokens;

    if (LLM_VOICE_MAX_TOKENS > 0 && v == AGENT_LLM_MAX_TOKENS_VOICE)
        v = LLM_VOICE_MAX_TOKENS;
    return v;
}

/* P0-NET：前缀缓存命中日志（DeepSeek prompt_cache_hit_tokens /
 * OpenAI prompt_tokens_details.cached_tokens / Anthropic cache_read_input_tokens）。
 * 只记录最近一次响应 usage；system prompt/工具 schema 保持稳定才有命中。 */
static int s_last_prompt_tokens;
static int s_last_cached_tokens;
static bool s_last_usage_valid;

static void llm_note_usage(cJSON* usage, int prompt_tokens)
{
    s_last_prompt_tokens = prompt_tokens;
    s_last_cached_tokens = 0;
    s_last_usage_valid = (prompt_tokens > 0);

    if (!usage)
        return;

    cJSON* hit = cJSON_GetObjectItem(usage, "prompt_cache_hit_tokens");
    if (hit && cJSON_IsNumber(hit)) {
        s_last_cached_tokens = (int)hit->valuedouble;
        return;
    }

    cJSON* det = cJSON_GetObjectItem(usage, "prompt_tokens_details");
    cJSON* cached = det ? cJSON_GetObjectItem(det, "cached_tokens") : NULL;
    if (cached && cJSON_IsNumber(cached)) {
        s_last_cached_tokens = (int)cached->valuedouble;
        return;
    }

    cJSON* cr = cJSON_GetObjectItem(usage, "cache_read_input_tokens");
    if (cr && cJSON_IsNumber(cr))
        s_last_cached_tokens = (int)cr->valuedouble;
}

static void llm_log_prefix_cache(const char* where)
{
    int pct = 0;

    if (!s_last_usage_valid)
        return;
    if (s_last_prompt_tokens > 0)
        pct = (int)((int64_t)s_last_cached_tokens * 100
            / s_last_prompt_tokens);
    syslog(LOG_INFO,
        "[lat] prefix_cache %s prompt_tokens=%d cached_tokens=%d hit=%d%%\n",
        where, s_last_prompt_tokens, s_last_cached_tokens, pct);
}

void llm_proxy_set_max_tokens(int v)
{
    s_max_tokens_override = (v > 0) ? v : 0;
}

/* P0-NET：保守预连接（由 agent_loop 在网络就绪时调用）。
 * 覆盖 LLM 当前 host + MiMo token-plan 网关 host（LLM/TTS/ASR 同一 host，
 * mimo_key_local.h 注释；两条相同则去重）。只做 TCP+TLS 握手并放回连接池，
 * 不发送请求；失败静默、不新增线程。返回 OK 表示至少一条暖连接就绪。 */
int llm_proxy_preconnect(void)
{
    char hosts[2][128];
    char ports[2][8];
    char llm_host[128];
    char llm_port[8];
    int n = 0;
    int ok = 0;

    pthread_mutex_lock(&s_llm_lock);
    memcpy(llm_host, s_llm_host, sizeof(llm_host));
    memcpy(llm_port, s_llm_port, sizeof(llm_port));
    pthread_mutex_unlock(&s_llm_lock);

    if (llm_host[0] != '\0') {
        strncpy(hosts[n], llm_host, sizeof(hosts[n]) - 1);
        hosts[n][sizeof(hosts[n]) - 1] = '\0';
        strncpy(ports[n], llm_port, sizeof(ports[n]) - 1);
        ports[n][sizeof(ports[n]) - 1] = '\0';
        n++;
    }

#ifdef MIMO_API_HOST
    if (n == 0 || strcmp(hosts[0], MIMO_API_HOST) != 0) {
        strncpy(hosts[n], MIMO_API_HOST, sizeof(hosts[n]) - 1);
        hosts[n][sizeof(hosts[n]) - 1] = '\0';
        strncpy(ports[n], MIMO_API_PORT, sizeof(ports[n]) - 1);
        ports[n][sizeof(ports[n]) - 1] = '\0';
        n++;
    }
#endif

    if (n == 0)
        return ERROR;

    for (int i = 0; i < n; i++) {
        int ret = vela_tls_preconnect(hosts[i], ports[i]);

        syslog(LOG_INFO, "[%s] [lat] preconnect %s:%s -> %s\n",
            TAG, hosts[i], ports[i], ret == 0 ? "ok" : "skip");
        if (ret == 0)
            ok++;
    }

    return ok > 0 ? OK : ERROR;
}

int llm_set_all(const char* host, const char* path,
    const char* port, const char* api_key, const char* model)
{
    pthread_mutex_lock(&s_llm_lock);

    /* v114b10（B 报告 P1）：每轮 LLM 配置重新 apply 时，值没变就跳过
     * claw_config_set（否则 5 键 × 整文件读 + NOR 写 + rename ≈ 3–4 秒/轮）。 */
    if (host && host[0] && strcmp(s_llm_host, host) != 0) {
        claw_config_set(AGENT_CFG_KEY_LLM_HOST, host);
        strncpy(s_llm_host, host, sizeof(s_llm_host) - 1);
        s_llm_host[sizeof(s_llm_host) - 1] = '\0';
    }
    if (path && path[0] && strcmp(s_llm_path, path) != 0) {
        claw_config_set(AGENT_CFG_KEY_LLM_PATH, path);
        strncpy(s_llm_path, path, sizeof(s_llm_path) - 1);
        s_llm_path[sizeof(s_llm_path) - 1] = '\0';
    }
    if (port && port[0] && strcmp(s_llm_port, port) != 0) {
        claw_config_set("llm_port", port);
        strncpy(s_llm_port, port, sizeof(s_llm_port) - 1);
        s_llm_port[sizeof(s_llm_port) - 1] = '\0';
    }
    if (api_key && api_key[0] && strcmp(s_api_key, api_key) != 0) {
        claw_config_set(AGENT_CFG_KEY_API_KEY, api_key);
        strncpy(s_api_key, api_key, sizeof(s_api_key) - 1);
        s_api_key[sizeof(s_api_key) - 1] = '\0';
    }
    if (model && model[0] && strcmp(s_model, model) != 0) {
        claw_config_set(AGENT_CFG_KEY_MODEL, model);
        strncpy(s_model, model, sizeof(s_model) - 1);
        s_model[sizeof(s_model) - 1] = '\0';
    }

    pthread_mutex_unlock(&s_llm_lock);

    syslog(LOG_INFO, "[%s] LLM config updated atomically: %s%s (model: %s)\n",
        TAG, s_llm_host, s_llm_path, s_model);
    return OK;
}

/* ── Config snapshot (used by llm_vision.c) ───────────────── */

void llm_snapshot_config(char* model, size_t model_sz,
    char* api_key, size_t key_sz,
    char* host, size_t host_sz)
{
    pthread_mutex_lock(&s_llm_lock);
    strncpy(model, s_model, model_sz - 1);
    model[model_sz - 1] = '\0';
    strncpy(api_key, s_api_key, key_sz - 1);
    api_key[key_sz - 1] = '\0';
    strncpy(host, s_llm_host, host_sz - 1);
    host[host_sz - 1] = '\0';
    pthread_mutex_unlock(&s_llm_lock);
}

/* ── Vision config snapshot ──────────────────────────────────── */

void llm_snapshot_vision_config(char* model, size_t model_sz,
    char* api_key, size_t key_sz,
    char* host, size_t host_sz)
{
    pthread_mutex_lock(&s_llm_lock);

    /* Use vision-specific config when set, otherwise fall back to main LLM */
    if (s_vision_model[0])
        strncpy(model, s_vision_model, model_sz - 1);
    else
        strncpy(model, s_model, model_sz - 1);
    model[model_sz - 1] = '\0';

    if (s_vision_api_key[0])
        strncpy(api_key, s_vision_api_key, key_sz - 1);
    else
        strncpy(api_key, s_api_key, key_sz - 1);
    api_key[key_sz - 1] = '\0';

    if (s_vision_host[0])
        strncpy(host, s_vision_host, host_sz - 1);
    else
        strncpy(host, s_llm_host, host_sz - 1);
    host[host_sz - 1] = '\0';

    pthread_mutex_unlock(&s_llm_lock);
}

/* ── Vision model setter ─────────────────────────────────────── */

int llm_set_vision_model(const char* host, const char* model,
    const char* api_key)
{
    pthread_mutex_lock(&s_llm_lock);

    if (host && host[0]) {
        claw_config_set(AGENT_CFG_KEY_VISION_HOST, host);
        strncpy(s_vision_host, host, sizeof(s_vision_host) - 1);
        s_vision_host[sizeof(s_vision_host) - 1] = '\0';
    } else {
        claw_config_set(AGENT_CFG_KEY_VISION_HOST, "");
        s_vision_host[0] = '\0';
    }

    if (model && model[0]) {
        claw_config_set(AGENT_CFG_KEY_VISION_MODEL, model);
        strncpy(s_vision_model, model, sizeof(s_vision_model) - 1);
        s_vision_model[sizeof(s_vision_model) - 1] = '\0';
    } else {
        claw_config_set(AGENT_CFG_KEY_VISION_MODEL, "");
        s_vision_model[0] = '\0';
    }

    if (api_key && api_key[0]) {
        claw_config_set(AGENT_CFG_KEY_VISION_API_KEY, api_key);
        strncpy(s_vision_api_key, api_key, sizeof(s_vision_api_key) - 1);
        s_vision_api_key[sizeof(s_vision_api_key) - 1] = '\0';
    } else {
        claw_config_set(AGENT_CFG_KEY_VISION_API_KEY, "");
        s_vision_api_key[0] = '\0';
    }

    syslog(LOG_INFO, "[%s] Vision LLM config updated: model=%s host=%s\n",
        TAG, s_vision_model[0] ? s_vision_model : "(inherit)",
        s_vision_host[0] ? s_vision_host : "(inherit)");

    pthread_mutex_unlock(&s_llm_lock);
    return OK;
}

/* ── HTTP helpers ─────────────────────────────────────────── */

/**
 * Return the model name to send in the API request.
 * Multi-provider gateways (e.g. OpenRouter) require the full
 * "provider/model" string, while single-vendor APIs expect only
 * the bare model name after the slash.
 */
const char* model_name_for_api(const char* model, const char* host)
{
    /* Multi-provider gateways need the full identifier */
    if (strstr(host, "openrouter.ai")) {
        return model;
    }

    const char* slash = strchr(model, '/');
    return slash ? slash + 1 : model;
}

/* Direct path via vela_tls */
static int llm_http_direct(const char* post_data, resp_buf_t* rb,
    int* out_status)
{
    /* Snapshot config under lock */
    char api_key[128], llm_host[128], llm_path[128], llm_port[8], model[64];
    pthread_mutex_lock(&s_llm_lock);
    memcpy(api_key, s_api_key, sizeof(api_key));
    memcpy(llm_host, s_llm_host, sizeof(llm_host));
    memcpy(llm_path, s_llm_path, sizeof(llm_path));
    memcpy(llm_port, s_llm_port, sizeof(llm_port));
    memcpy(model, s_model, sizeof(model));
    pthread_mutex_unlock(&s_llm_lock);

    /* Allocate the response buffer once and hand it directly to the
     * TLS layer.  Previously we allocated a separate raw_buf and then
     * copied into resp_buf — doubling peak memory usage.  Now the
     * resp_buf IS the raw buffer, eliminating the copy. */
    size_t raw_cap = AGENT_LLM_STREAM_BUF_SIZE;
    if (resp_buf_init(rb, raw_cap) != OK)
        return ERROR;

    /* OpenAI-compatible format: Authorization: Bearer <key> */
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    /* Extract provider ID from model string (e.g. "xiaomi/mimo-claw-0301" →
     * "xiaomi") Mify gateway requires X-Model-Provider-Id header for routing. */
    char provider[64] = { 0 };
    const char* slash = strchr(model, '/');
    if (slash) {
        size_t plen = (size_t)(slash - model);
        if (plen >= sizeof(provider))
            plen = sizeof(provider) - 1;
        memcpy(provider, model, plen);
    }

    vela_header_t hdrs[] = { { "Authorization", auth_header },
        { provider[0] ? "X-Model-Provider-Id" : NULL,
            provider[0] ? provider : NULL },
        { NULL, NULL } };

    int status;
    int use_tls = (strcmp(llm_port, "443") == 0);

    /* 2026-11-01 v102d：与 TTS 会话互斥，避免 BT PAN 上两条 TLS 流并发 */
    voice_channel_llm_begin();
    if (use_tls) {
        status = vela_https_post_json(llm_host, llm_port, llm_path, hdrs, post_data,
            rb->data, raw_cap);
    } else {
        status = vela_http_post_json(llm_host, llm_port, llm_path, hdrs, post_data,
            rb->data, raw_cap);
    }
    voice_channel_llm_end();

    if (status < 0) {
        resp_buf_free(rb);
        return ERROR;
    }

    /* Update length — TLS layer NUL-terminated the buffer */
    rb->len = strlen(rb->data);

    *out_status = status;
    return OK;
}

/* Proxy path via CONNECT tunnel */
static int llm_http_via_proxy(const char* post_data, resp_buf_t* rb,
    int* out_status)
{
    /* Snapshot config under lock */
    char api_key[128], llm_host[128], llm_path[128], llm_port[8], model[64];
    pthread_mutex_lock(&s_llm_lock);
    memcpy(api_key, s_api_key, sizeof(api_key));
    memcpy(llm_host, s_llm_host, sizeof(llm_host));
    memcpy(llm_path, s_llm_path, sizeof(llm_path));
    memcpy(llm_port, s_llm_port, sizeof(llm_port));
    memcpy(model, s_model, sizeof(model));
    pthread_mutex_unlock(&s_llm_lock);

    int port = atoi(llm_port);
    if (port <= 0)
        port = 443;

    proxy_conn_t* conn = proxy_conn_open(llm_host, port, 30000);
    if (!conn)
        return ERROR;

    int body_len = (int)strlen(post_data);
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    /* Extract provider ID from model (e.g. "xiaomi/mimo-claw-0301" → "xiaomi") */
    char provider_hdr[128] = { 0 };
    const char* slash = strchr(model, '/');
    if (slash) {
        char provider[64] = { 0 };
        size_t plen = (size_t)(slash - model);
        if (plen >= sizeof(provider))
            plen = sizeof(provider) - 1;
        memcpy(provider, model, plen);
        snprintf(provider_hdr, sizeof(provider_hdr), "X-Model-Provider-Id: %s\r\n",
            provider);
    }

    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Authorization: %s\r\n"
        "%s"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        llm_path, llm_host, auth_header, provider_hdr, body_len);

    if (proxy_conn_write(conn, header, hlen) < 0 || proxy_conn_write(conn, post_data, body_len) < 0) {
        proxy_conn_close(conn);
        return ERROR;
    }

    if (resp_buf_init(rb, AGENT_LLM_STREAM_BUF_SIZE) != OK) {
        proxy_conn_close(conn);
        return ERROR;
    }

    char tmp[4096];
    while (1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 120000);
        if (n <= 0)
            break;
        if (resp_buf_append(rb, tmp, (size_t)n) != OK) {
            syslog(LOG_ERR, "[%s] resp_buf_append OOM, truncating\n", TAG);
            break;
        }
    }
    proxy_conn_close(conn);

    /* Parse status from raw HTTP response */
    *out_status = 0;
    if (rb->len > 5 && strncmp(rb->data, "HTTP/", 5) == 0) {
        const char* sp = strchr(rb->data, ' ');
        if (sp)
            *out_status = atoi(sp + 1);
    }

    /* Strip HTTP header, keep body */
    char* body = strstr(rb->data, "\r\n\r\n");
    if (body) {
        body += 4;
        size_t blen = rb->len - (size_t)(body - rb->data);
        memmove(rb->data, body, blen);
        rb->len = blen;
        rb->data[rb->len] = '\0';
    }

    return OK;
}

int llm_http_call(const char* post_data, resp_buf_t* rb,
    int* out_status)
{
    /* For plain HTTP endpoints (port != 443), always use direct path.
     * The proxy does CONNECT + TLS which fails on non-TLS endpoints.
     * Non-TLS HTTP endpoints (port != 443) are reachable
     * directly without a proxy anyway. */
    int use_tls;
    pthread_mutex_lock(&s_llm_lock);
    use_tls = (strcmp(s_llm_port, "443") == 0);
    pthread_mutex_unlock(&s_llm_lock);

#ifdef CONFIG_AI_AGENT_NET_RPMSG
    int retry_max = network_get_retry_max();
    int retry_base = network_get_retry_base_sec();
    int ret = ERROR;

    for (int attempt = 0; attempt <= retry_max; attempt++) {
        if (attempt > 0) {
            /* Check network state before retrying */
            if (network_get_state() != NET_STATE_CONNECTED) {
                syslog(LOG_WARNING, "[llm] Network down, skip retry %d\n", attempt);
                break;
            }
            int delay = retry_base * (1 << (attempt - 1));
            syslog(LOG_INFO, "[llm] Retry %d/%d after %ds\n", attempt, retry_max, delay);
            sleep(delay);
        }

        if (use_tls && http_proxy_is_enabled()) {
            ret = llm_http_via_proxy(post_data, rb, out_status);
        } else {
            ret = llm_http_direct(post_data, rb, out_status);
        }

        if (ret == OK) {
            return OK;
        }

        syslog(LOG_WARNING, "[llm] HTTP call failed (attempt %d/%d)\n",
            attempt + 1, retry_max + 1);
    }

    /* All retries failed — notify user */
    syslog(LOG_ERR, "[llm] All %d retries failed\n", retry_max + 1);
    {
        agent_msg_t notify;
        memset(&notify, 0, sizeof(notify));
        strncpy(notify.channel, "cli", sizeof(notify.channel) - 1);
        /* 错误提示：不是回答，禁止进 TTS (P2a)。cli 通道当前不出声，这里
         * 打标是为了将来分享 speak 路径时行为仍然正确。 */
        notify.kind = AGENT_MSG_KIND_ERROR;
        notify.content = strdup("网络请求失败，请检查蓝牙连接");
        if (notify.content) {
            if (message_bus_push_outbound(&notify) != OK) {
                free(notify.content);
            }
        }
    }
    return ERROR;
#else
    if (use_tls && http_proxy_is_enabled())
        return llm_http_via_proxy(post_data, rb, out_status);
    return llm_http_direct(post_data, rb, out_status);
#endif
}

/* 2026-10-26: MiMo 即使请求里带 thinking:disabled, 仍偶发把"内心思考"写进
 * 正文, 例如:
 *   "用户又让我用三句话介绍自己。这已经是第二次了。我是毒舌人设, 但也要
 *    注意不能太过分。这次要保持简洁, 直接三句话搞定。"
 * 这些文字会被显示并**逐字念出来**, 听感完全坏掉(真机 22:25 用户反馈)。
 * 这里做保守过滤: 只从头剥离"明显是思考句"的整句, 遇到正常回答立即停止。
 */
static bool reasoning_like_sentence(const char* s, size_t len)
{
    static const char* k_markers[] = {
        "用户要求", "用户又让", "用户又", "用户让", "用户问",
        "用户说", "用户希望", "按照persona", "按照人设",
        "这已经是第", "这次要", "保持毒舌", "直接三句话",
        "人设要求", "我需要先", "作为毒舌", "回答：",
        "最终回答：", NULL };

    if (len == 0)
        return false;

    for (int i = 0; k_markers[i] != NULL; i++)
    {
        size_t mlen = strlen(k_markers[i]);
        if (len >= mlen && memcmp(s, k_markers[i], mlen) == 0)
            return true;
    }

    /* 句首就是"用户…"也算思考句 */
    if (len >= 6 && memcmp(s, "用户", 6) == 0)
        return true;

    /* MiMo 有时直接用英文写思考过程(真机 22:34 实测):
     * "The user keeps asking the same question repeatedly. This is getting
     *  annoying. As a 毒舌 persona, I should call this out..." */
    static const char* k_en_markers[] = {
        "The user", "This is getting", "As a ", "I should",
        "I need to", "Let me ", "We need", "Okay,", "They asked",
        NULL };

    if (len >= 3)
    {
        for (int i = 0; k_en_markers[i] != NULL; i++)
        {
            size_t mlen = strlen(k_en_markers[i]);
            if (len >= mlen && memcmp(s, k_en_markers[i], mlen) == 0)
                return true;
        }
    }

    return false;
}

static void strip_reasoning_prefix(char* text)
{
    if (text == NULL || text[0] == '\0')
        return;

    char* p = text;

    /* 最多剥 4 句, 防止把正常回答吃掉 */
    for (int iter = 0; iter < 4; iter++)
    {
        size_t remain = strlen(p);
        size_t end = 0;

        while (end < remain && end < 240)
        {
            char c = p[end];
            if (c == '。' || c == '！' || c == '？' || c == '\n'
                || c == '!' || c == '?')
                break;
            end++;
        }

        if (end >= remain || end >= 240)
            break;

        size_t sentence_len = end + 1;

        if (!reasoning_like_sentence(p, sentence_len))
            break;

        p += sentence_len;
        while (*p == ' ' || *p == '\n' || *p == '\r')
            p++;
    }

    if (p != text && *p != '\0')
    {
        memmove(text, p, strlen(p) + 1);
    }
}

/* ── JSON helpers ─────────────────────────────────────────── */

/* Extract text from OpenAI response format: choices[0].message.content */
void extract_text(cJSON* root, char* buf, size_t size)
{
    buf[0] = '\0';
    cJSON* choices = cJSON_GetObjectItem(root, "choices");
    if (!choices || !cJSON_IsArray(choices))
        return;

    cJSON* first = choices->child;
    if (!first)
        return;

    cJSON* message = cJSON_GetObjectItem(first, "message");
    if (!message)
        return;

    cJSON* content = cJSON_GetObjectItem(message, "content");
    if (!content || !cJSON_IsString(content))
        return;

    size_t tlen = strlen(content->valuestring);
    size_t copy = (tlen < size - 1) ? tlen : size - 1;
    memcpy(buf, content->valuestring, copy);
    buf[copy] = '\0';

    /* 去掉模型写在正文开头的"思考过程"(见上方注释), 避免被念出来。 */
    strip_reasoning_prefix(buf);
}

/* ── Public: simple chat ──────────────────────────────────── */

int llm_chat(const char* system_prompt, const char* messages_json,
    char* response_buf, size_t buf_size)
{
    /* Snapshot config under lock */
    char model[64], api_key[128], llm_host[128];
    pthread_mutex_lock(&s_llm_lock);
    memcpy(model, s_model, sizeof(model));
    memcpy(api_key, s_api_key, sizeof(api_key));
    memcpy(llm_host, s_llm_host, sizeof(llm_host));
    pthread_mutex_unlock(&s_llm_lock);

    if (api_key[0] == '\0') {
        snprintf(response_buf, buf_size, "Error: No API key configured");
        return ERROR;
    }

    cJSON* body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model",
        model_name_for_api(model, llm_host));

    if (is_openai_compat_host(llm_host))
        cJSON_AddNumberToObject(body, "max_completion_tokens",
            llm_effective_max_tokens(AGENT_LLM_MAX_TOKENS_OPENAI));
    else
        cJSON_AddNumberToObject(body, "max_tokens",
            llm_effective_max_tokens(AGENT_LLM_MAX_TOKENS));

    cJSON* messages = cJSON_Parse(messages_json);
    if (!messages)
        messages = cJSON_CreateArray();

    /* Prepend system message (OpenAI format) */
    cJSON* sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_InsertItemInArray(messages, 0, sys_msg);

    cJSON_AddItemToObject(body, "messages", messages);

    if (host_is_mimo(llm_host)) {
        add_mimo_thinking_disabled(body);
    }

    char* post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) {
        snprintf(response_buf, buf_size, "Error: Failed to build request");
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Calling LLM API (model: %s, host: %s, %d bytes)\n",
        TAG, model, llm_host, (int)strlen(post_data));

    resp_buf_t rb = { 0 };
    int status = 0;
    int err = ERROR;
    int retry;

    for (retry = 0; retry <= AGENT_LLM_MAX_RETRIES; retry++) {
        if (retry > 0) {
            unsigned int delay = AGENT_LLM_RETRY_BASE_SEC << (retry - 1);
            syslog(LOG_WARNING, "[%s] Rate limited (429), retry %d/%d after %us\n",
                TAG, retry, AGENT_LLM_MAX_RETRIES, delay);
            sleep(delay);
        }

        rb.len = 0;
        status = 0;
        err = llm_http_call(post_data, &rb, &status);

        if (err != OK) {
            resp_buf_free(&rb);
            free(post_data);
            snprintf(response_buf, buf_size, "Error: HTTP request failed");
            return err;
        }

        if (status != 429)
            break;

        resp_buf_free(&rb);
        memset(&rb, 0, sizeof(rb));
    }

    free(post_data);

    if (status != 200) {
        snprintf(response_buf, buf_size, "API error (HTTP %d): %.200s", status,
            rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return ERROR;
    }

    cJSON* root = cJSON_Parse(rb.data);
    resp_buf_free(&rb);

    if (!root) {
        snprintf(response_buf, buf_size, "Error: Failed to parse response");
        return ERROR;
    }

    extract_text(root, response_buf, buf_size);
    cJSON_Delete(root);

    if (response_buf[0] == '\0')
        snprintf(response_buf, buf_size, "No response from LLM API");
    else
        syslog(LOG_INFO, "[%s] LLM response: %d bytes\n", TAG,
            (int)strlen(response_buf));

    return OK;
}

/* ── Public: chat with tools ──────────────────────────────── */


int llm_chat_tools(const char* system_prompt, cJSON* messages,
    const char* tools_json, llm_response_t* resp)
{
    memset(resp, 0, sizeof(*resp));

    /* Snapshot config under lock */
    char model[64];
    char api_key[128];
    char llm_host[128];

    pthread_mutex_lock(&s_llm_lock);
    memcpy(model, s_model, sizeof(model));
    memcpy(api_key, s_api_key, sizeof(api_key));
    memcpy(llm_host, s_llm_host, sizeof(llm_host));
    pthread_mutex_unlock(&s_llm_lock);

    if (api_key[0] == '\0') {
        return ERROR;
    }

    cJSON* body = cJSON_CreateObject();

    cJSON_AddStringToObject(body, "model",
        model_name_for_api(model, llm_host));

    if (is_openai_compat_host(llm_host)) {
        cJSON_AddNumberToObject(body, "max_completion_tokens",
            llm_effective_max_tokens(AGENT_LLM_MAX_TOKENS_OPENAI));
    } else {
        cJSON_AddNumberToObject(body, "max_tokens",
            llm_effective_max_tokens(AGENT_LLM_MAX_TOKENS));
    }

    /* Clone messages and prepend system message */
    cJSON* msgs = cJSON_Duplicate(messages, 1);
    cJSON* sys_msg = cJSON_CreateObject();

    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_InsertItemInArray(msgs, 0, sys_msg);
    cJSON_AddItemToObject(body, "messages", msgs);

    if (host_is_mimo(llm_host)) {
        add_mimo_thinking_disabled(body);
    }

    /* Convert tools to OpenAI format */
    cJSON* tools_arr = build_openai_tools_array(tools_json);

    if (tools_arr) {
        cJSON_AddItemToObject(body, "tools", tools_arr);
    }

    char* post_data = cJSON_PrintUnformatted(body);

    cJSON_Delete(body);
    if (!post_data) {
        return ERROR;
    }

    syslog(LOG_INFO,
        "[%s] OpenAI API with tools (model: %s, %d bytes)\n",
        TAG, model, (int)strlen(post_data));

    resp_buf_t rb = { 0 };
    int status = 0;
    int err = ERROR;
    int retry;

    for (retry = 0; retry <= AGENT_LLM_MAX_RETRIES; retry++) {
        if (retry > 0) {
            unsigned int delay = AGENT_LLM_RETRY_BASE_SEC << (retry - 1);
            syslog(LOG_WARNING, "[%s] Rate limited (429), retry %d/%d after %us\n",
                TAG, retry, AGENT_LLM_MAX_RETRIES, delay);
            sleep(delay);
        }

        rb.len = 0;
        status = 0;
        err = llm_http_call(post_data, &rb, &status);

        if (err != OK) {
            resp_buf_free(&rb);
            free(post_data);
            return err;
        }

        if (status != 429)
            break;

        resp_buf_free(&rb);
        memset(&rb, 0, sizeof(rb));
    }

    free(post_data);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] API error %d: %.500s\n", TAG, status,
            rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return ERROR;
    }

    cJSON* root = cJSON_Parse(rb.data);

    resp_buf_free(&rb);
    if (!root) {
        syslog(LOG_ERR, "[%s] Failed to parse API JSON\n", TAG);
        return ERROR;
    }

    /* Extract from OpenAI response */
    cJSON* choices = cJSON_GetObjectItem(root, "choices");

    if (!choices || !cJSON_IsArray(choices) || !choices->child) {
        cJSON_Delete(root);
        return ERROR;
    }

    cJSON* choice = choices->child;
    cJSON* finish = cJSON_GetObjectItem(choice, "finish_reason");

    resp->tool_use = (finish && cJSON_IsString(finish)
        && strcmp(finish->valuestring, "tool_calls") == 0);

    cJSON* message = cJSON_GetObjectItem(choice, "message");

    if (message) {
        cJSON* text_content = cJSON_GetObjectItem(message, "content");
        if (text_content && cJSON_IsString(text_content) && text_content->valuestring) {
            size_t tlen = strlen(text_content->valuestring);
            resp->text = calloc(1, tlen + 1);
            if (resp->text) {
                memcpy(resp->text, text_content->valuestring, tlen);
                resp->text_len = tlen;

                /* 2026-10-26: 工具路径也必须过滤"思考过程"正文 —— MiMo 会把
                 * "The user keeps asking... / 用户又让我..." 写进 content，
                 * 这里剥掉前导思考句，避免被显示/播报。 */
                strip_reasoning_prefix(resp->text);
                resp->text_len = strlen(resp->text);
            }
        }

        /* Kimi thinking mode: preserve reasoning_content so it can be echoed
         * back in the next turn (required by the API). */
        cJSON* rc = cJSON_GetObjectItem(message, "reasoning_content");
        if (rc && cJSON_IsString(rc) && rc->valuestring && rc->valuestring[0]) {
            resp->reasoning_content = strdup(rc->valuestring);
        }

        /* Extract tool calls from OpenAI format */
        cJSON* tool_calls = cJSON_GetObjectItem(message, "tool_calls");
        if (tool_calls && cJSON_IsArray(tool_calls)) {
            cJSON* tc;
            cJSON_ArrayForEach(tc, tool_calls)
            {
                if (resp->call_count >= AGENT_MAX_TOOL_CALLS)
                    break;

                llm_tool_call_t* call = &resp->calls[resp->call_count];

                /* OpenAI tool_calls format:
                   { "id": "call_xyz", "type": "function",
                     "function": { "name": "foo", "arguments": "{...}" }
                   }
                */
                cJSON* id_item = cJSON_GetObjectItem(tc, "id");
                if (id_item && cJSON_IsString(id_item))
                    strncpy(call->id, id_item->valuestring, sizeof(call->id) - 1);

                cJSON* func_obj = cJSON_GetObjectItem(tc, "function");
                if (func_obj) {
                    cJSON* name_item = cJSON_GetObjectItem(func_obj, "name");
                    cJSON* args_item = cJSON_GetObjectItem(func_obj, "arguments");

                    if (name_item && cJSON_IsString(name_item))
                        strncpy(call->name, name_item->valuestring, sizeof(call->name) - 1);

                    if (args_item && cJSON_IsString(args_item)) {
                        size_t alen = strlen(args_item->valuestring);
                        call->input = calloc(1, alen + 1);
                        if (call->input) {
                            memcpy(call->input, args_item->valuestring, alen);
                            call->input_len = alen;
                        }
                    } else if (args_item && cJSON_IsObject(args_item)) {
                        /* Some models return arguments as a JSON object
                         * instead of a JSON string — serialize it. */
                        char* serialized = cJSON_PrintUnformatted(args_item);
                        if (serialized) {
                            call->input = serialized;
                            call->input_len = strlen(serialized);
                        }
                    }
                }

                resp->call_count++;
            }

            /* If we found tool calls, ensure tool_use flag is set
             * (some models/proxies return stop_reason="end_turn" even with tools) */
            if (resp->call_count > 0) {
                resp->tool_use = true;
            }
        }
    }

    /* Extract token usage from API response */
    cJSON* usage = cJSON_GetObjectItem(root, "usage");
    if (usage) {
        cJSON* pt = cJSON_GetObjectItem(usage, "prompt_tokens");
        cJSON* ct = cJSON_GetObjectItem(usage, "completion_tokens");
        cJSON* tt = cJSON_GetObjectItem(usage, "total_tokens");
        if (pt && cJSON_IsNumber(pt))
            resp->prompt_tokens = (int)pt->valuedouble;
        if (ct && cJSON_IsNumber(ct))
            resp->completion_tokens = (int)ct->valuedouble;
        if (tt && cJSON_IsNumber(tt))
            resp->total_tokens = (int)tt->valuedouble;
        /* P0-NET：记录前缀缓存命中（cached_tokens）并输出 [lat] 日志 */
        llm_note_usage(usage, resp->prompt_tokens);
    }

    /* XML fallback parsers for non-standard models */
    parse_xml_tool_calls(resp);
    parse_ns_xml_tool_calls(resp);

    cJSON_Delete(root);

    llm_log_prefix_cache("chat");

    syslog(LOG_INFO,
        "[%s] Response: %d bytes text, %d tool calls, finish=%s\n",
        TAG, (int)resp->text_len, resp->call_count,
        resp->tool_use ? "tool_calls" : "end_turn");

    return OK;
}


/* ═══════════════════════════════════════════════════════════════
 * v130+ SPEED-A：LLM 流式（SSE）路径
 *
 * 目标：语音/聊天回答不再等完整响应 —— 首个 delta 到达就回调上层上屏，
 * 文字首字 1-2s 可见。非流式 llm_chat_tools() 保持原样作为回退路径：
 *   - HTTP 非 200 / 连接失败 / 流式解析失败 / 服务端不支持 SSE
 *     → 自动重发一次非流式请求（最终回答以完整 JSON 为准）。
 * 代价：回退时多一次 LLM 往返（重复计费/多 2-8s）；已显示的半成品由
 *      上层用最终文本覆盖（agent_loop 的 stream_end）。
 * ═══════════════════════════════════════════════════════════════ */

#define LLM_SSE_LINE_MAX        4096   /* 单条 SSE 行（一个 JSON chunk）上限 */
#define LLM_STREAM_TOOL_ARGS_MAX 4096  /* 单个 tool_call arguments 上限 */
/* 流式文本上限：16KB 对 110~256 token 的回答绰绰有余，同时给 heap 兜底
 * （本板 SRAM 紧张，不能像非流式那样放到 512KB）。超限按流式失败处理，
 * 由上层回退非流式。 */
#define LLM_STREAM_TEXT_MAX      (16 * 1024)
#define LLM_STREAM_REASON_MAX    (8 * 1024)

/* ── VOICE-A（docs_ble/112 §2A；VOICE-A-FIX 按 perf-fw 复核分离预算）──
 * 语音/离群回合实测首 token 4–45s，会把语音回合拖到 50s+。这里把两类超时分开：
 *   - first-token deadline（LLM_FIRST_TOKEN_DEADLINE_MS=8000，**仅语音回合**）：
 *     首个 content delta 超过它才到 -> 中止流/关闭连接、返回 -ETIMEDOUT 并
 *     跳过非流式回退；agent_loop 对 -ETIMEDOUT 也跳过 router failover，直接
 *     走 local_lm/短句（perf-fw A2）。
 *   - 生成总预算（LLM_STREAM_TOTAL_DEADLINE_MS=25000，仅语音回合）：给首 token
 *     之后的生成阶段充裕时间，避免 90 字回答在 8s 被截断（perf-fw A3）；
 *     到点仍有 partial 时按成功保留（partial 只作异常兜底）。
 * 计时起点 t_start 在 voice_channel_llm_begin() 之后打点（perf-fw A1）：不再把
 * 等 TTS 会话（上限 30s）算进首 token 8s；TLS 连接/握手仍计入（空池首连
 * 2–8s，池化复用后约 0）。
 * 非语音通道（ble/cli 256 token 可能 >8s）保持原 90s 总预算，且不做首 token
 * 检查（perf-fw A4）。
 * 回滚开关：-DLLM_FIRST_TOKEN_DEADLINE_MS=0（关首 token 降级）；
 *           -DLLM_STREAM_TOTAL_DEADLINE_MS=0（语音沿用 90s 总预算）。
 */
#ifndef LLM_FIRST_TOKEN_DEADLINE_MS
#define LLM_FIRST_TOKEN_DEADLINE_MS 8000
#endif
#ifndef LLM_STREAM_TOTAL_DEADLINE_MS
#define LLM_STREAM_TOTAL_DEADLINE_MS 25000
#endif

/* ── RCA-FIX-A（audio_rca2 F1，2026-11-04）：首字节主动 deadline ──
 * 旧实现只在 SSE 解析到第一个 content delta 时才检查 8s deadline；服务端完全
 * 静默时只能等 vela 的 20s 流空闲超时/25s 总预算（实测 18:10 首 token
 * 17.357s 才触发）。这里给语音回合传 first_body_timeout_ms：在**首个 body
 * 字节到达前**，vela 读循环/socket 读超时按该预算收紧，到点直接 -ETIMEDOUT。
 * 非语音通道传 0（保持 90s 预算与旧行为）；生成阶段仍用 25s 总预算，互不影响。
 * 回滚：-DLLM_FIRST_BODY_DEADLINE_MS=0（关闭首字节主动超时，回到事件驱动）。 */
#ifndef LLM_FIRST_BODY_DEADLINE_MS
#define LLM_FIRST_BODY_DEADLINE_MS 9000
#endif

static int64_t llm_proxy_mono_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

typedef struct {
    char* line;
    size_t line_cap;
    size_t line_len;

    llm_delta_cb_t cb;
    void* cb_arg;
    llm_response_t* resp;

    char* text;         /* 累积最终文本（成功时转交给 resp->text） */
    size_t text_len;
    size_t text_cap;

    char* reasoning;    /* Kimi thinking：streamed reasoning_content 需回传 */
    size_t reasoning_len;
    size_t reasoning_cap;
    bool reasoning_cut; /* 思考内容被截断：tool 轮回退非流式以保证正确 */

    int delta_count;
    int parse_errors;
    bool failed;        /* 内存/超长行等不可恢复错误 */
    int fail_err;

    /* VOICE-A：首 token 超时判定 */
    int64_t t_start_ms;        /* 首 token 计时起点（voice_channel_llm_begin 之后） */
    long    first_token_ms;    /* 首个 content delta 耗时；-1 = 尚未收到 */
    bool    first_token_timeout; /* 1 = 首 token 超过 deadline，已中止流 */
    bool    first_token_check; /* 只对语音回合做首 token 检查（perf-fw A4） */
} llm_sse_t;

/* 释放流式期间累积到 resp 的 partial 文本/tool_calls，避免回退时泄漏。 */
static void llm_stream_resp_reset(llm_sse_t* st)
{
    llm_response_t* resp = st->resp;

    if (st->text) {
        free(st->text);
        st->text = NULL;
        st->text_len = 0;
        st->text_cap = 0;
    }
    if (st->reasoning) {
        free(st->reasoning);
        st->reasoning = NULL;
        st->reasoning_len = 0;
        st->reasoning_cap = 0;
    }
    if (resp->text && resp->text != st->text) {
        free(resp->text);
    }
    resp->text = NULL;
    resp->text_len = 0;
    if (resp->reasoning_content) {
        free(resp->reasoning_content);
        resp->reasoning_content = NULL;
    }
    for (int i = 0; i < AGENT_MAX_TOOL_CALLS; i++) {
        if (resp->calls[i].input) {
            free(resp->calls[i].input);
            resp->calls[i].input = NULL;
        }
    }
    memset(resp, 0, sizeof(*resp));
}

static int llm_sse_buf_append(char** dst, size_t* len, size_t* cap,
    const char* s, size_t max)
{
    size_t add = strlen(s);
    size_t need = *len + add + 1;

    if (need > max) {
        return ERROR;
    }
    if (*cap < need) {
        size_t ncap = *cap ? *cap : 256;
        char* nb;

        while (ncap < need)
            ncap *= 2;
        if (ncap > max)
            ncap = max;
        nb = realloc(*dst, ncap);
        if (!nb)
            return ERROR;
        *dst = nb;
        *cap = ncap;
    }
    memcpy(*dst + *len, s, add);
    *len += add;
    (*dst)[*len] = '\0';
    return OK;
}

static int llm_sse_text_append(llm_sse_t* st, const char* s)
{
    size_t add = strlen(s);
    size_t need = st->text_len + add + 1;

    if (need > LLM_STREAM_TEXT_MAX) {
        st->failed = true;
        st->fail_err = -EOVERFLOW;
        return ERROR;
    }
    if (llm_sse_buf_append(&st->text, &st->text_len, &st->text_cap, s,
            LLM_STREAM_TEXT_MAX)
        != OK) {
        st->failed = true;
        st->fail_err = -ENOMEM;
        return ERROR;
    }
    return OK;
}

/* tool_calls 分片按 index 拼接：id/name 可能缺失（重复只取第一次出现的），
 * arguments 是真正的增量，必须追加。 */
static void llm_sse_tool_delta(llm_sse_t* st, cJSON* tcs)
{
    cJSON* tc;

    cJSON_ArrayForEach(tc, tcs)
    {
        int index = 0;
        cJSON* idx = cJSON_GetObjectItem(tc, "index");
        cJSON* id;
        cJSON* fn;
        llm_tool_call_t* call;

        if (idx && cJSON_IsNumber(idx))
            index = (int)idx->valuedouble;
        if (index < 0 || index >= AGENT_MAX_TOOL_CALLS) {
            st->parse_errors++;
            continue;
        }
        call = &st->resp->calls[index];

        id = cJSON_GetObjectItem(tc, "id");
        if (id && cJSON_IsString(id) && id->valuestring
            && id->valuestring[0] && call->id[0] == '\0') {
            strncpy(call->id, id->valuestring, sizeof(call->id) - 1);
        }

        fn = cJSON_GetObjectItem(tc, "function");
        if (fn) {
            cJSON* name = cJSON_GetObjectItem(fn, "name");
            cJSON* args = cJSON_GetObjectItem(fn, "arguments");

            if (name && cJSON_IsString(name) && name->valuestring
                && name->valuestring[0]) {
                size_t cur = strlen(call->name);
                size_t add = strlen(name->valuestring);

                if (cur + add < sizeof(call->name)) {
                    memcpy(call->name + cur, name->valuestring, add + 1);
                } else {
                    st->parse_errors++;
                }
            }
            if (args && cJSON_IsString(args) && args->valuestring) {
                size_t add = strlen(args->valuestring);

                if (call->input_len + add > LLM_STREAM_TOOL_ARGS_MAX) {
                    st->parse_errors++;
                } else {
                    char* nb = realloc(call->input,
                        call->input_len + add + 1);
                    if (!nb) {
                        st->failed = true;
                        st->fail_err = -ENOMEM;
                    } else {
                        call->input = nb;
                        memcpy(call->input + call->input_len,
                            args->valuestring, add);
                        call->input_len += add;
                        call->input[call->input_len] = '\0';
                    }
                }
            }
        }

        if (index + 1 > st->resp->call_count)
            st->resp->call_count = index + 1;
        st->resp->tool_use = true;
    }
}

/* 处理一行（已去 \r）。只关心 "data: {...}"；空行/注释/其它 SSE 字段忽略。 */
static void llm_sse_process_line(llm_sse_t* st, char* line)
{
    char* p;
    cJSON* root;
    cJSON* choices;

    if (line[0] == '\0' || line[0] == ':')
        return;
    if (strncmp(line, "data:", 5) != 0)
        return;

    p = line + 5;
    while (*p == ' ')
        p++;

    if (strcmp(p, "[DONE]") == 0)
        return;   /* 正常收尾；payload 由 chunked framing 自然结束 */

    root = cJSON_Parse(p);
    if (!root) {
        st->parse_errors++;
        return;
    }

    choices = cJSON_GetObjectItem(root, "choices");
    if (choices && cJSON_IsArray(choices)) {
        cJSON* ch = choices->child;
        if (ch) {
            cJSON* delta = cJSON_GetObjectItem(ch, "delta");
            cJSON* finish = cJSON_GetObjectItem(ch, "finish_reason");

            if (delta) {
                cJSON* content = cJSON_GetObjectItem(delta, "content");
                if (content && cJSON_IsString(content)
                    && content->valuestring && content->valuestring[0]) {
                    /* VOICE-A：首个 content delta 即"首 token"。超过 deadline
                     * 才到 -> 标记失败（feed 返回负值让 vela 中止流/关闭连接），
                     * llm_chat_tools_stream 会返回 -ETIMEDOUT 并跳过非流式回退，
                     * agent_loop 既有失败路径直接回落 local。 */
                    if (st->delta_count == 0) {
                        long fd_ms = (long)(llm_proxy_mono_ms()
                            - st->t_start_ms);

                        /* RCA-FIX-A F8：首个 content delta 到达时刻（所有通道；
                         * 语音回合同时用它判定首 token deadline）。 */
                        syslog(LOG_INFO,
                            "[%s] [lat] llm first_delta_ms=%ld\n",
                            TAG, fd_ms);
                        st->first_token_ms = fd_ms;
                        if (st->first_token_check
                            && LLM_FIRST_TOKEN_DEADLINE_MS > 0
                            && st->first_token_ms
                                >= LLM_FIRST_TOKEN_DEADLINE_MS) {
                            st->first_token_timeout = true;
                            st->failed = true;
                            st->fail_err = -ETIMEDOUT;
                            syslog(LOG_WARNING,
                                "[%s] first-token timeout %ldms (>= %dms), "
                                "abort streaming and close connection\n",
                                TAG, st->first_token_ms,
                                LLM_FIRST_TOKEN_DEADLINE_MS);
                            return;
                        }
                    }
                    if (llm_sse_text_append(st, content->valuestring) == OK) {
                        st->delta_count++;
                        if (st->cb)
                            st->cb(content->valuestring, st->cb_arg);
                    }
                }
                {
                    cJSON* rc = cJSON_GetObjectItem(delta, "reasoning_content");
                    if (rc && cJSON_IsString(rc) && rc->valuestring
                        && rc->valuestring[0]) {
                        if (llm_sse_buf_append(&st->reasoning,
                                &st->reasoning_len, &st->reasoning_cap,
                                rc->valuestring, LLM_STREAM_REASON_MAX)
                            != OK)
                            st->reasoning_cut = true;
                    }
                }
                {
                    cJSON* tcs = cJSON_GetObjectItem(delta, "tool_calls");
                    if (tcs && cJSON_IsArray(tcs))
                        llm_sse_tool_delta(st, tcs);
                }
            }
            if (finish && cJSON_IsString(finish) && finish->valuestring
                && finish->valuestring[0]) {
                if (strcmp(finish->valuestring, "tool_calls") == 0)
                    st->resp->tool_use = true;
            }
        }
    }

    /* usage 可能只在最后一个 chunk 出现（部分服务端总是带） */
    {
        cJSON* usage = cJSON_GetObjectItem(root, "usage");
        if (usage) {
            cJSON* pt = cJSON_GetObjectItem(usage, "prompt_tokens");
            cJSON* ct = cJSON_GetObjectItem(usage, "completion_tokens");
            cJSON* tt = cJSON_GetObjectItem(usage, "total_tokens");

            if (pt && cJSON_IsNumber(pt))
                st->resp->prompt_tokens = (int)pt->valuedouble;
            if (ct && cJSON_IsNumber(ct))
                st->resp->completion_tokens = (int)ct->valuedouble;
            if (tt && cJSON_IsNumber(tt))
                st->resp->total_tokens = (int)tt->valuedouble;
            /* P0-NET：流式 usage 里同样可能带 cached_tokens */
            llm_note_usage(usage, st->resp->prompt_tokens);
        }
    }

    cJSON_Delete(root);
}

/* TLS 流式回调：按行缓冲，拼出完整 SSE 行再解析。
 * 返回负值会让 vela_tls 中止本次流（上层回退非流式）。 */
static int llm_sse_feed(const char* data, size_t len, void* arg)
{
    llm_sse_t* st = (llm_sse_t*)arg;
    size_t i;

    for (i = 0; i < len; i++) {
        char c = data[i];

        if (c == '\n') {
            size_t l = st->line_len;

            if (l > 0 && st->line[l - 1] == '\r')
                l--;
            st->line[l] = '\0';
            llm_sse_process_line(st, st->line);
            st->line_len = 0;
            if (st->failed)
                return st->fail_err ? st->fail_err : -ENOMEM;
        } else {
            if (st->line_len + 1 >= st->line_cap) {
                /* 超长行：不能安全解析，交给上层回退非流式 */
                st->failed = true;
                st->fail_err = -EOVERFLOW;
                return st->fail_err;
            }
            st->line[st->line_len++] = c;
        }
    }
    return 0;
}

/* 流式 HTTP 调用（仅 TLS 直连）。返回 OK 时 out_status 为 HTTP 状态码。
 * 代理/明文 HTTP 不支持流式，返回 ERROR 让上层走非流式（与现有
 * llm_http_call 的代理路径保持同一行为）。 */
static int llm_http_call_stream(const char* post_data,
    vela_http_stream_cb_t cb, void* cb_arg, int* out_status,
    uint32_t timeout_ms, uint32_t first_body_timeout_ms,
    int64_t* io_t_start_ms)
{
    char api_key[128], llm_host[128], llm_path[128], llm_port[8], model[64];
    char auth_header[256];
    char provider[64] = { 0 };
    const char* slash;
    int status;
    int use_tls;

    pthread_mutex_lock(&s_llm_lock);
    memcpy(api_key, s_api_key, sizeof(api_key));
    memcpy(llm_host, s_llm_host, sizeof(llm_host));
    memcpy(llm_path, s_llm_path, sizeof(llm_path));
    memcpy(llm_port, s_llm_port, sizeof(llm_port));
    memcpy(model, s_model, sizeof(model));
    pthread_mutex_unlock(&s_llm_lock);

    use_tls = (strcmp(llm_port, "443") == 0);
    if (!use_tls || http_proxy_is_enabled()) {
        syslog(LOG_INFO,
            "[%s] stream unsupported (tls=%d proxy=%d), use non-stream\n",
            TAG, use_tls, http_proxy_is_enabled());
        return ERROR;
    }

    snprintf(auth_header, sizeof(auth_header), "Bearer %s", api_key);

    slash = strchr(model, '/');
    if (slash) {
        size_t plen = (size_t)(slash - model);

        if (plen >= sizeof(provider))
            plen = sizeof(provider) - 1;
        memcpy(provider, model, plen);
    }

    {
        vela_header_t hdrs[] = { { "Authorization", auth_header },
            { provider[0] ? "X-Model-Provider-Id" : NULL,
                provider[0] ? provider : NULL },
            { NULL, NULL } };

        /* 与 TTS 会话互斥（同 llm_http_direct），整个流期间持有。 */
        voice_channel_llm_begin();

        /* VOICE-A-FIX（perf-fw A1）：首 token 计时从**等完 TTS 会话之后**才开始，
         * 否则 voice_channel_llm_begin() 最长 30s 的等待会被算进 8s deadline，
         * 正常 6-8s 首 token 的回合会被误杀。TLS 连接/握手仍在计时内（池化
         * 复用后约 0；空池首连 2-8s 属于真实网络成本）。 */
        if (io_t_start_ms != NULL)
            *io_t_start_ms = llm_proxy_mono_ms();

        /* VOICE-A / RCA-FIX-A：timeout_ms = 生成总预算（语音 25s / 其它 90s）；
         * first_body_timeout_ms = 首个 body 字节前的主动 deadline（语音 9s，
         * 非语音 0=旧行为）。两者独立：首字节到达后只用总预算。 */
        if (first_body_timeout_ms > 0) {
            status = vela_https_post_json_stream_ex(llm_host, llm_port,
                llm_path, hdrs, post_data, cb, cb_arg, timeout_ms,
                first_body_timeout_ms);
        } else {
            status = vela_https_post_json_stream(llm_host, llm_port, llm_path,
                hdrs, post_data, cb, cb_arg, timeout_ms);
        }
        voice_channel_llm_end();
    }

    if (status < 0) {
        syslog(LOG_WARNING, "[%s] stream POST failed: %d\n", TAG, status);
        return ERROR;
    }

    *out_status = status;
    return OK;
}

int llm_chat_tools_stream(const char* system_prompt, cJSON* messages,
    const char* tools_json, llm_response_t* resp,
    llm_delta_cb_t cb, void* cb_arg)
{
    char model[64];
    char api_key[128];
    char llm_host[128];
    cJSON* body;
    cJSON* msgs;
    cJSON* sys_msg;
    cJSON* tools_arr;
    char* post_data;
    llm_sse_t st;
    int status = 0;
    int http_err;
    int result = ERROR;
    bool usable = false;
    bool voice_mode = false;
    uint32_t stream_timeout_ms;
    uint32_t first_body_timeout_ms = 0;
    long elapsed_ms = 0;
    int max_tokens_ov = 0;

    memset(resp, 0, sizeof(*resp));

    if (!cb) {
        /* 没有回调就等于非流式（agent_loop 的极简回退） */
        return llm_chat_tools(system_prompt, messages, tools_json, resp);
    }

    pthread_mutex_lock(&s_llm_lock);
    memcpy(model, s_model, sizeof(model));
    memcpy(api_key, s_api_key, sizeof(api_key));
    memcpy(llm_host, s_llm_host, sizeof(llm_host));
    max_tokens_ov = s_max_tokens_override;
    pthread_mutex_unlock(&s_llm_lock);

    if (api_key[0] == '\0')
        return ERROR;

    /* VOICE-A-FIX（perf-fw A3/A4）：agent_loop 在语音回合用
     * llm_proxy_set_max_tokens(AGENT_LLM_MAX_TOKENS_VOICE) 标记通道。
     * 语音回合：流式请求总预算 = LLM_STREAM_TOTAL_DEADLINE_MS(25s)，首 token
     * 由 8s deadline 单独判定；其它通道保持 90s 总预算且不做首 token 检查。 */
    voice_mode = (max_tokens_ov > 0
        && max_tokens_ov == AGENT_LLM_MAX_TOKENS_VOICE);
    stream_timeout_ms = (voice_mode && LLM_STREAM_TOTAL_DEADLINE_MS > 0)
        ? (uint32_t)LLM_STREAM_TOTAL_DEADLINE_MS
        : (uint32_t)AGENT_LLM_TIMEOUT_SEC * 1000U;
    /* RCA-FIX-A F1：语音回合在首个 body 字节前用 9s 主动 deadline；
     * 非语音=0（完全旧行为，90s 总预算不受影响）。 */
    first_body_timeout_ms = (voice_mode && LLM_FIRST_BODY_DEADLINE_MS > 0)
        ? (uint32_t)LLM_FIRST_BODY_DEADLINE_MS : 0;

    /* 请求体：与 llm_chat_tools() 完全一致 + "stream": true */
    body = cJSON_CreateObject();
    cJSON_AddStringToObject(body, "model", model_name_for_api(model, llm_host));

    if (is_openai_compat_host(llm_host)) {
        cJSON_AddNumberToObject(body, "max_completion_tokens",
            llm_effective_max_tokens(AGENT_LLM_MAX_TOKENS_OPENAI));
    } else {
        cJSON_AddNumberToObject(body, "max_tokens",
            llm_effective_max_tokens(AGENT_LLM_MAX_TOKENS));
    }

    msgs = cJSON_Duplicate(messages, 1);
    sys_msg = cJSON_CreateObject();
    cJSON_AddStringToObject(sys_msg, "role", "system");
    cJSON_AddStringToObject(sys_msg, "content", system_prompt);
    cJSON_InsertItemInArray(msgs, 0, sys_msg);
    cJSON_AddItemToObject(body, "messages", msgs);

    if (host_is_mimo(llm_host))
        add_mimo_thinking_disabled(body);

    tools_arr = build_openai_tools_array(tools_json);
    if (tools_arr)
        cJSON_AddItemToObject(body, "tools", tools_arr);

    cJSON_AddBoolToObject(body, "stream", true);

    post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data)
        return ERROR;

    syslog(LOG_INFO,
        "[%s] LLM stream request (model: %s, tools=%d, %d bytes)\n", TAG,
        model, tools_arr != NULL, (int)strlen(post_data));

    memset(&st, 0, sizeof(st));
    st.cb = cb;
    st.cb_arg = cb_arg;
    st.resp = resp;
    st.line_cap = LLM_SSE_LINE_MAX;
    st.line = malloc(st.line_cap);
    if (!st.line) {
        free(post_data);
        return ERROR;
    }
    st.line[0] = '\0';
    st.first_token_ms = -1;
    st.first_token_check = voice_mode;   /* perf-fw A4：只有语音回合做首 token 检查 */
    st.t_start_ms = llm_proxy_mono_ms(); /* 兜底；真实起点在 llm_http_call_stream
                                          * 里 voice_channel_llm_begin() 之后覆盖 */

    /* P0-NET：记录建连序号，结束时据此判断本轮是否真的新建连接（否则=复用 cache）。 */
    uint64_t conn_seq0 = vela_tls_conn_seq();

    http_err = llm_http_call_stream(post_data, llm_sse_feed, &st, &status,
        stream_timeout_ms, first_body_timeout_ms, &st.t_start_ms);
    free(post_data);

    elapsed_ms = (long)(llm_proxy_mono_ms() - st.t_start_ms);

    /* VOICE-A：首 token 超时（迟到的 delta 在 parser 里已经中止流/关闭连接）。
     * 直接返回 -ETIMEDOUT，**跳过非流式回退**，让 agent_loop 的失败路径立刻
     * 回落 local_lm/短句，而不是再等一次非流式请求。 */
    if (st.first_token_timeout) {
        syslog(LOG_WARNING,
            "[%s] first-token timeout %ldms (deadline %dms): return -ETIMEDOUT "
            "without non-stream fallback\n",
            TAG, elapsed_ms, LLM_FIRST_TOKEN_DEADLINE_MS);
        free(st.line);
        llm_stream_resp_reset(&st);
        return -ETIMEDOUT;
    }

    if (http_err == OK && status == 200) {
        if (!st.failed && st.parse_errors == 0
            && (st.text_len > 0
                || (resp->tool_use && resp->call_count > 0))) {
            usable = true;
        } else if (!st.failed && st.parse_errors == 0 && st.text_len > 0) {
            usable = true;
        } else {
            syslog(LOG_WARNING,
                "[%s] stream unusable (failed=%d parse_errors=%d text=%zu calls=%d), fallback\n",
                TAG, st.failed, st.parse_errors, st.text_len,
                resp->call_count);
        }
    } else {
        syslog(LOG_WARNING,
            "[%s] stream failed (http_err=%d status=%d), fallback to non-stream\n",
            TAG, http_err, status);
    }

    /* VOICE-A-FIX（perf-fw A4）：一个 delta 都没收到且已到首 token deadline
     * （语音回合的静默/慢首 token；此时流已被 vela 侧读超时/回退关闭）->
     * 按首 token 超时处理：返回 -ETIMEDOUT、不重发非流式。
     * 非语音通道保持旧行为（不走首 token 降级）。 */
    if (!usable && voice_mode && st.delta_count == 0
        && LLM_FIRST_TOKEN_DEADLINE_MS > 0
        && elapsed_ms >= LLM_FIRST_TOKEN_DEADLINE_MS) {
        syslog(LOG_WARNING,
            "[%s] first-token timeout %ldms (deadline %dms, no delta): return "
            "-ETIMEDOUT without non-stream fallback\n",
            TAG, elapsed_ms, LLM_FIRST_TOKEN_DEADLINE_MS);
        free(st.line);
        llm_stream_resp_reset(&st);
        return -ETIMEDOUT;
    }

    if (usable && resp->tool_use && st.reasoning_cut) {
        /* Kimi thinking：tool 轮必须把完整 reasoning_content 回传，截断会
         * 400。这里宁可多一次非流式请求，也不把坏数据交给下一轮。 */
        syslog(LOG_WARNING,
            "[%s] streaming reasoning truncated on tool round, fallback\n",
            TAG);
        usable = false;
    }

    /* VOICE-A-FIX（perf-fw A3）：首 token 与生成总预算分离后，这里只兜底
     * "生成阶段 25s 总预算到点"这一异常情况：首 token 已到达（delta_count>0）
     * 的回合保留已收到的部分文本按成功返回，不整轮回退 local；正常 90 字
     * 回答在 25s 内生成完，不再被 8s 截断（partial 仅异常兜底）。 */
    if (!usable && voice_mode && LLM_STREAM_TOTAL_DEADLINE_MS > 0
        && st.delta_count > 0 && st.text_len > 0 && st.parse_errors == 0
        && !st.failed && elapsed_ms >= LLM_STREAM_TOTAL_DEADLINE_MS) {
        syslog(LOG_WARNING,
            "[%s] generation budget hit after first token (deltas=%d text=%zu B), "
            "keep partial answer\n",
            TAG, st.delta_count, st.text_len);
        usable = true;
    }

    if (usable) {
        /* 文本/思考内容接管：st.* -> resp.* */
        resp->text = st.text;
        resp->text_len = st.text_len;
        st.text = NULL;
        if (st.reasoning && st.reasoning_len > 0) {
            resp->reasoning_content = st.reasoning;
            st.reasoning = NULL;
        }

        if (resp->text && resp->text_len > 0) {
            strip_reasoning_prefix(resp->text);
            resp->text_len = strlen(resp->text);
        }
        if (resp->tool_use && resp->call_count > 0)
            resp->tool_use = true;

        syslog(LOG_INFO,
            "[%s] stream done: deltas=%d text=%zu tool_use=%d calls=%d\n",
            TAG, st.delta_count, resp->text_len, resp->tool_use,
            resp->call_count);
        result = OK;
    } else {
        /* 回退：丢弃 partial，重发一次非流式请求 */
        llm_stream_resp_reset(&st);
        result = llm_chat_tools(system_prompt, messages, tools_json, resp);
        syslog(LOG_INFO, "[%s] stream fallback non-stream result: %s\n", TAG,
            result == OK ? "OK" : "FAILED");
    }

    /* P0-NET：[lat] LLM 分段延迟（首 token / 生成 / 连接 / 握手）。
     * ttft/gen 只在流式成功时有意义；回退非流式时按 -1 记录。
     * 连接耗时只在本轮**真的新建连接**时有效（conn_seq 变化）；否则是复用
     * 池连接（cache）：conn/handshake 记 0，pool=1（perf-fw 复核要求）。 */
    {
        int64_t conn_ms = 0;
        int64_t hs_ms = 0;
        bool pool_hit = true;
        long ttft_ms = (st.first_token_ms > 0) ? st.first_token_ms : -1;
        long gen_ms = (ttft_ms > 0 && elapsed_ms >= ttft_ms)
            ? (elapsed_ms - ttft_ms) : -1;

        if (vela_tls_conn_seq() != conn_seq0) {
            bool hit = true;

            vela_tls_get_last_timing(&conn_ms, &hs_ms, &hit);
            pool_hit = hit;
            if (pool_hit) {   /* 预连接/其它线程已建好：本轮仍是复用 */
                conn_ms = 0;
                hs_ms = 0;
            }
        }
        syslog(LOG_INFO,
            "[lat] llm ttft_ms=%ld gen_ms=%ld deltas=%d text=%zu "
            "conn=%s conn_ms=%lld handshake_ms=%lld\n",
            ttft_ms, gen_ms, st.delta_count, resp->text_len,
            pool_hit ? "cache" : "new", (long long)conn_ms, (long long)hs_ms);
    }
    llm_log_prefix_cache("stream");

    free(st.line);
    return result;
}

