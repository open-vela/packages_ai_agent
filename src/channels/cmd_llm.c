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

#include "channels/cmd_llm.h"
#include "infra/config_store.h"
#include "infra/url_parse.h"
#include "llm/llm_proxy.h"
#include "llm/llm_router.h"
#include "infra/http_proxy.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"
#include "agent_config.h"

#include <cJSON.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Router presets (shared by set_llm and router_set) ─────────── */

typedef struct {
    const char* name;
    const char* host;
    const char* path;
    const char* model;
    int cost_tier;
} router_preset_t;

static const router_preset_t g_router_presets[] = {
    { "deepseek", "api.deepseek.com", "/v1/chat/completions", "deepseek-chat", 1 },
    { "kimi", "api.moonshot.cn", "/v1/chat/completions", "kimi-k2.5", 2 },
    { "qwen", "dashscope.aliyuncs.com", "/compatible-mode/v1/chat/completions",
        "qwen-turbo", 1 },
    { "glm", "open.bigmodel.cn", "/api/paas/v4/chat/completions", "glm-4-flash", 1 },
    { "openai", "api.openai.com", "/v1/chat/completions", "gpt-4o", 3 },
    { "claude", "api.anthropic.com", "/v1/messages", "claude-sonnet-4-20250514", 3 },
    { "mimo", "api.xiaomimimo.com", "/v1/chat/completions", "mimo-v2-flash", 1 },
    { "openrouter", "openrouter.ai", "/api/v1/chat/completions",
        "openrouter/hunter-alpha", 2 },
    { NULL, NULL, NULL, NULL, 0 }
};

/* ── cmd_set_llm ──────────────────────────────────────────────── */

void cmd_set_llm(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_llm <preset> [api_key]\n"
               "       set_llm <url> <model> [api_key]\n"
               "       set_llm <host> <model> [api_key]\n"
               "\n"
               "Presets:\n"
               "  kimi     - api.moonshot.cn  (kimi-k2.5)\n"
               "  qwen     - dashscope.aliyuncs.com  (qwen-turbo)\n"
               "  deepseek - api.deepseek.com  (deepseek-chat)\n"
               "  glm      - open.bigmodel.cn  (glm-4-flash)\n"
               "  openai   - api.openai.com  (gpt-4o)\n"
               "  openrouter - openrouter.ai  (openrouter/hunter-alpha)\n"
               "  mimo     - api.xiaomimimo.com  (MiMo-v2-Flash)\n"
               "\n"
               "URL format (for custom endpoints):\n"
               "  set_llm http://host:port/path model key\n"
               "  set_llm http://<your-endpoint>/v1 model-name sk-xxx\n"
               "\n"
               "Note: set_llm writes to router slot 0 and applies immediately.\n");
        return;
    }

    const char* arg1 = argv[1];

    const char* host = NULL;
    const char* port = "443";
    const char* path = "/v1/chat/completions";
    const char* model = NULL;
    const char* api_key = NULL;
    int cost_tier = 1;

    /* Check if arg1 is a URL (http:// or https://) */
    if (strncmp(arg1, "http://", 7) == 0 || strncmp(arg1, "https://", 8) == 0) {
        parsed_url_t parsed;
        if (url_parse(arg1, &parsed) != 0) {
            printf("Invalid URL: %s\n", arg1);
            return;
        }
        host = parsed.host;
        port = parsed.port;

        /* Extract path from URL */
        if (parsed.path[0] && parsed.path[1]) {
            path = parsed.path;
        } else {
            path = "/v1/chat/completions";
        }

        /* Append /chat/completions if path is just /v1 */
        char full_path[256];
        if (strcmp(path, "/v1") == 0 || strcmp(path, "/v1/") == 0) {
            snprintf(full_path, sizeof(full_path), "/v1/chat/completions");
            path = full_path;
        }

        if (argc >= 3)
            model = argv[2];
        if (argc >= 4)
            api_key = argv[3];
    }
    /* Check presets — reuse g_router_presets table */
    else {
        const router_preset_t* preset = NULL;
        for (int i = 0; g_router_presets[i].name; i++) {
            if (strcmp(g_router_presets[i].name, arg1) == 0) {
                preset = &g_router_presets[i];
                break;
            }
        }

        if (preset) {
            host = preset->host;
            path = preset->path;
            model = preset->model;
            cost_tier = preset->cost_tier;
            port = "443";
            if (argc >= 3)
                api_key = argv[2];
        } else {
            /* Custom: arg1 is bare host */
            host = arg1;
            port = "443";
            if (argc >= 3)
                model = argv[2];
            if (argc >= 4)
                api_key = argv[3];
        }
    }

    /* Build backend and write to router slot 0 */
    llm_backend_t backend = { 0 };
    strncpy(backend.host, host, sizeof(backend.host) - 1);
    backend.host[sizeof(backend.host) - 1] = '\0';
    strncpy(backend.path, path, sizeof(backend.path) - 1);
    backend.path[sizeof(backend.path) - 1] = '\0';
    strncpy(backend.port, port, sizeof(backend.port) - 1);
    backend.port[sizeof(backend.port) - 1] = '\0';
    if (model) {
        strncpy(backend.model, model, sizeof(backend.model) - 1);
        backend.model[sizeof(backend.model) - 1] = '\0';
    }

    /* Preserve existing API key if not provided */
    if (api_key) {
        strncpy(backend.api_key, api_key, sizeof(backend.api_key) - 1);
        backend.api_key[sizeof(backend.api_key) - 1] = '\0';
    } else {
        llm_backend_t old;
        if (llm_router_get_backend(0, &old) == 0 && old.api_key[0]) {
            strncpy(backend.api_key, old.api_key, sizeof(backend.api_key) - 1);
            backend.api_key[sizeof(backend.api_key) - 1] = '\0';
        }
    }

    backend.priority = 0;
    backend.cost_tier = cost_tier;
    backend.enabled = true;

    /* Write to router slot 0 and apply immediately */
    llm_router_set_backend(0, &backend);
    llm_router_apply(0);

    printf("LLM backend: %s:%s%s (model: %s) [router slot 0]\n",
        host, port, path, model ? model : "(unchanged)");
    if (api_key) {
        printf("API key saved.\n");
    }
}


/* ── cmd_set_vision_llm ──────────────────────────────────────── */

void cmd_set_vision_llm(int argc, char** argv)
{
    if (argc < 2) {
        printf("Usage: set_vision_llm <preset> [api_key]\n"
               "       set_vision_llm <host> <model> [api_key]\n"
               "       set_vision_llm clear\n"
               "\n"
               "Set an independent vision model. If not configured,\n"
               "vision calls inherit the main LLM config.\n"
               "\n"
               "Presets:\n"
               "  mimo     - api.xiaomimimo.com  (mimo-v2-omni)\n"
               "  openai   - api.openai.com  (gpt-4o)\n"
               "  qwen     - dashscope.aliyuncs.com  (qwen-vl-max)\n"
               "  glm      - open.bigmodel.cn  (glm-4v-flash)\n"
               "\n"
               "Examples:\n"
               "  set_vision_llm mimo <api_key>\n"
               "  set_vision_llm clear\n");
        return;
    }

    const char* arg1 = argv[1];

    /* clear: remove vision-specific config, fall back to main LLM */
    if (strcmp(arg1, "clear") == 0) {
        llm_set_vision_model(NULL, NULL, NULL);
        printf("Vision LLM config cleared (using main LLM).\n");
        return;
    }

    const char* host = NULL;
    const char* model = NULL;
    const char* api_key = NULL;

    /* Check if arg1 is a preset name */
    static const struct {
        const char* name;
        const char* host;
        const char* model;
    } vision_presets[] = {
        { "mimo", "api.xiaomimimo.com", "mimo-v2-omni" },
        { "openai", "api.openai.com", "gpt-4o" },
        { "qwen", "dashscope.aliyuncs.com", "qwen-vl-max" },
        { "glm", "open.bigmodel.cn", "glm-4v-flash" },
        { NULL, NULL, NULL }
    };

    bool found = false;
    for (int i = 0; vision_presets[i].name; i++) {
        if (strcmp(vision_presets[i].name, arg1) == 0) {
            host = vision_presets[i].host;
            model = vision_presets[i].model;
            if (argc >= 3)
                api_key = argv[2];
            found = true;
            break;
        }
    }

    if (!found) {
        /* Custom: arg1 = host, arg2 = model, arg3 = api_key */
        host = arg1;
        if (argc >= 3)
            model = argv[2];
        if (argc >= 4)
            api_key = argv[3];
    }

    llm_set_vision_model(host, model, api_key);

    printf("Vision LLM: %s (model: %s)\n", host, model ? model : "(inherit)");
    if (api_key)
        printf("Vision API key saved.\n");
}


/* ── list_models helpers ──────────────────────────────────────── */

#define LIST_MODELS_BUF_SIZE (256 * 1024)
#define LIST_MODELS_PATH "/api/v1/models"

static int list_models_fetch(const char* api_key, char** out, size_t* out_len)
{
    char* buf = malloc(LIST_MODELS_BUF_SIZE);

    if (!buf) {
        printf("Error: OOM allocating response buffer\n");
        return ERROR;
    }

    vela_header_t hdrs[3];
    char auth[160];

    snprintf(auth, sizeof(auth), "Bearer %s", api_key);
    hdrs[0].name = "Authorization";
    hdrs[0].value = auth;
    hdrs[1].name = "Accept";
    hdrs[1].value = "application/json";
    hdrs[2].name = NULL;
    hdrs[2].value = NULL;

    size_t body_len;
    int status = vela_https_request(
        AGENT_LLM_OPENROUTER_HOST, "443", "GET",
        LIST_MODELS_PATH, hdrs, NULL, 0,
        buf, LIST_MODELS_BUF_SIZE, &body_len);

    if (status < 200 || status >= 300) {
        printf("Error: HTTP %d from models API\n", status);
        free(buf);
        return ERROR;
    }

    *out = buf;
    *out_len = body_len;
    return OK;
}

static int list_models_fetch_proxy(const char* api_key,
    char** out, size_t* out_len)
{
    proxy_conn_t* conn = proxy_conn_open(
        AGENT_LLM_OPENROUTER_HOST, 443, 30000);

    if (!conn) {
        printf("Error: proxy connection failed\n");
        return ERROR;
    }

    char hdr[512];
    int hlen = snprintf(hdr, sizeof(hdr),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Authorization: Bearer %s\r\n"
        "Accept: application/json\r\n"
        "Connection: close\r\n\r\n",
        LIST_MODELS_PATH, AGENT_LLM_OPENROUTER_HOST, api_key);

    if (proxy_conn_write(conn, hdr, hlen) < 0) {
        proxy_conn_close(conn);
        printf("Error: write failed\n");
        return ERROR;
    }

    char* buf = malloc(LIST_MODELS_BUF_SIZE);

    if (!buf) {
        proxy_conn_close(conn);
        printf("Error: OOM\n");
        return ERROR;
    }

    size_t total = 0;
    char tmp[4096];

    while (total < LIST_MODELS_BUF_SIZE - 1) {
        int n = proxy_conn_read(conn, tmp, sizeof(tmp), 60000);

        if (n <= 0) {
            break;
        }
        size_t avail = LIST_MODELS_BUF_SIZE - 1 - total;
        size_t copy = (size_t)n < avail ? (size_t)n : avail;

        memcpy(buf + total, tmp, copy);
        total += copy;
    }
    proxy_conn_close(conn);
    buf[total] = '\0';

    /* Strip HTTP header */
    char* body = strstr(buf, "\r\n\r\n");

    if (body) {
        body += 4;
        size_t blen = total - (size_t)(body - buf);
        memmove(buf, body, blen);
        buf[blen] = '\0';
        *out_len = blen;
    } else {
        *out_len = total;
    }

    *out = buf;
    return OK;
}

static bool model_is_free(cJSON* item)
{
    cJSON* pricing = cJSON_GetObjectItem(item, "pricing");

    if (!pricing) {
        return false;
    }

    const cJSON* prompt = cJSON_GetObjectItem(pricing, "prompt");
    const cJSON* completion = cJSON_GetObjectItem(pricing, "completion");

    if (!prompt || !completion) {
        return false;
    }

    const char* p = prompt->valuestring;
    const char* c = completion->valuestring;

    if (!p || !c) {
        return false;
    }
    return (atof(p) == 0.0 && atof(c) == 0.0);
}

static void list_models_print(cJSON* data, bool free_only,
    const char* keyword)
{
    int count = 0;
    int total = cJSON_GetArraySize(data);

    for (int i = 0; i < total; i++) {
        cJSON* item = cJSON_GetArrayItem(data, i);
        cJSON* id = cJSON_GetObjectItem(item, "id");

        if (!id || !id->valuestring) {
            continue;
        }

        bool is_free = model_is_free(item);

        if (free_only && !is_free) {
            continue;
        }
        if (keyword && !strstr(id->valuestring, keyword)) {
            continue;
        }

        printf("  %-45s %s\n", id->valuestring,
            is_free ? "[FREE]" : "[PAID]");
        count++;
    }

    printf("\n%d model(s) shown (total: %d)\n", count, total);
}

void cmd_list_models(int argc, char** argv)
{
    char host[128];
    char api_key[128];

    claw_config_get(AGENT_CFG_KEY_LLM_HOST, host, sizeof(host));
    claw_config_get(AGENT_CFG_KEY_API_KEY, api_key, sizeof(api_key));

    if (!strstr(host, "openrouter.ai")) {
        printf("list_models only works with openrouter.ai backend.\n"
               "Use: set_llm openrouter\n");
        return;
    }
    if (!api_key[0]) {
        printf("No API key set. Use: set_llm <preset> <key>\n");
        return;
    }

    bool free_only = false;
    const char* keyword = NULL;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--free") == 0) {
            free_only = true;
        } else {
            keyword = argv[i];
        }
    }

    printf("Fetching models from %s ...\n", AGENT_LLM_OPENROUTER_HOST);

    char* buf;
    size_t buf_len;
    int ret;

    if (http_proxy_is_enabled()) {
        ret = list_models_fetch_proxy(api_key, &buf, &buf_len);
    } else {
        ret = list_models_fetch(api_key, &buf, &buf_len);
    }
    if (ret != OK) {
        return;
    }

    cJSON* root = cJSON_Parse(buf);

    free(buf);
    buf = NULL;

    if (!root) {
        printf("Error: failed to parse JSON response\n");
        return;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");

    if (!data || !cJSON_IsArray(data)) {
        printf("Error: unexpected response format\n");
        cJSON_Delete(root);
        return;
    }

    list_models_print(data, free_only, keyword);
    cJSON_Delete(root);
}


/* ── Router commands ──────────────────────────────────────────── */

void cmd_router_status(void)
{
    char* json = llm_router_status_json();
    if (json) {
        printf("=== LLM Router Status ===\n%s\n=========================\n", json);
        free(json);
    } else {
        printf("Failed to get router status.\n");
    }
}

void cmd_router_profile(int argc, char** argv)
{
    if (argc < 2) {
        llm_route_profile_t p = llm_router_get_profile();
        const char* name = "auto";
        if (p == LLM_ROUTE_ECO)
            name = "eco";
        else if (p == LLM_ROUTE_PREMIUM)
            name = "premium";
        printf("Current profile: %s\n", name);
        printf("Usage: router_profile <eco|auto|premium>\n");
        return;
    }

    llm_route_profile_t profile = LLM_ROUTE_AUTO;
    const char* profile_name = "auto";
    if (strcmp(argv[1], "eco") == 0) {
        profile = LLM_ROUTE_ECO;
        profile_name = "eco";
    } else if (strcmp(argv[1], "premium") == 0) {
        profile = LLM_ROUTE_PREMIUM;
        profile_name = "premium";
    } else if (strcmp(argv[1], "auto") != 0) {
        printf("Unknown profile: %s (valid: eco, auto, premium)\n", argv[1]);
        return;
    }

    llm_router_set_profile(profile);
    printf("Router profile set to: %s\n", profile_name);
}

void cmd_router_set(int argc, char** argv)
{
    if (argc < 3) {
        printf("Usage: router_set <preset> <api_key>\n\n"
               "Presets:\n"
               "  deepseek  - DeepSeek (cheap, fast)\n"
               "  kimi      - Moonshot Kimi (mid-tier)\n"
               "  qwen      - Alibaba Qwen (cheap)\n"
               "  glm       - Zhipu GLM-4 (cheap)\n"
               "  openai    - OpenAI GPT-4o (premium)\n"
               "  claude    - Anthropic Claude (premium)\n"
               "  mimo      - Xiaomi MiMo (cheap)\n\n"
               "Example:\n"
               "  router_set deepseek sk-xxx\n"
               "  router_set openai sk-xxx\n");
        return;
    }

    const char* preset_name = argv[1];
    const char* api_key = argv[2];
    const router_preset_t* preset = NULL;

    for (int i = 0; g_router_presets[i].name; i++) {
        if (strcmp(g_router_presets[i].name, preset_name) == 0) {
            preset = &g_router_presets[i];
            break;
        }
    }

    if (!preset) {
        printf("Unknown preset: %s\n", preset_name);
        return;
    }

    /* Find first empty slot or reuse existing */
    int slot = -1;
    for (int i = 0; i < LLM_ROUTER_MAX_BACKENDS; i++) {
        llm_backend_t b;
        if (llm_router_get_backend(i, &b) != 0 || !b.enabled) {
            slot = i;
            break;
        }
        if (strcmp(b.host, preset->host) == 0) {
            slot = i; /* Update existing */
            break;
        }
    }

    if (slot < 0) {
        printf("Error: All 4 slots full. Use router_clear first.\n");
        return;
    }

    llm_backend_t backend = { 0 };
    strncpy(backend.host, preset->host, sizeof(backend.host) - 1);
    strncpy(backend.path, preset->path, sizeof(backend.path) - 1);
    strncpy(backend.port, "443", sizeof(backend.port) - 1);
    strncpy(backend.model, preset->model, sizeof(backend.model) - 1);
    strncpy(backend.api_key, api_key, sizeof(backend.api_key) - 1);
    backend.priority = slot;
    backend.cost_tier = preset->cost_tier;
    backend.enabled = true;

    if (llm_router_set_backend(slot, &backend) == 0) {
        printf("Added [%d]: %s (%s, tier=%d)\n",
            slot, preset_name, preset->model, preset->cost_tier);
    } else {
        printf("Failed to add backend.\n");
    }
}

void cmd_router_clear(int argc, char** argv)
{
    if (argc >= 2) {
        /* Validate numeric input */
        const char* arg = argv[1];
        if (arg[0] < '0' || arg[0] > '9') {
            printf("Error: index must be a number (0-%d)\n",
                LLM_ROUTER_MAX_BACKENDS - 1);
            return;
        }
        int idx = atoi(arg);
        if (idx >= 0 && idx < LLM_ROUTER_MAX_BACKENDS) {
            llm_backend_t empty = { 0 };
            llm_router_set_backend(idx, &empty);
            printf("Slot %d cleared.\n", idx);
        } else {
            printf("Error: index must be 0-%d\n", LLM_ROUTER_MAX_BACKENDS - 1);
        }
    } else {
        for (int i = 0; i < LLM_ROUTER_MAX_BACKENDS; i++) {
            llm_backend_t empty = { 0 };
            llm_router_set_backend(i, &empty);
        }
        printf("All router backends cleared.\n");
    }
}

void cmd_router_model(int argc, char** argv)
{
    if (argc < 3) {
        printf("Usage: router_model <index> <model_name>\n"
               "Example: router_model 1 gpt-4o-mini\n");
        return;
    }

    int idx = atoi(argv[1]);
    if (idx < 0 || idx >= LLM_ROUTER_MAX_BACKENDS) {
        printf("Error: index must be 0-%d\n", LLM_ROUTER_MAX_BACKENDS - 1);
        return;
    }

    llm_backend_t b;
    if (llm_router_get_backend(idx, &b) != 0 || !b.enabled) {
        printf("Error: slot %d is empty\n", idx);
        return;
    }

    strncpy(b.model, argv[2], sizeof(b.model) - 1);
    b.model[sizeof(b.model) - 1] = '\0';

    if (llm_router_set_backend(idx, &b) == 0) {
        printf("Slot %d model changed to: %s\n", idx, argv[2]);
    } else {
        printf("Failed to update model.\n");
    }
}
