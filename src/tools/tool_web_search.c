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

#include "tools/tool_web_search.h"
#include "agent_config.h"
#include "agent_compat.h"
#include "infra/http_proxy.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "infra/weather_cache.h"   /* 进程级共享天气缓存（天气页/后台预取共用） */

#include <string.h>
#include <stdlib.h>
#include <stddef.h>
#include <time.h>
#include "cJSON.h"

static const char *TAG = "web_search";

/* ── 天气结果 10 分钟缓存 ────────────────────────────────────────
 * 天气是低速变化数据; 现场演示时同一句会反复问, 而慢网(TLS+HTTP 往返)
 * 一次要 30s+。命中缓存直接返回上次的中文结果, 零网络开销。
 * 只缓存"同一 location"的最终文本。 */
#define WS_WX_TTL_MS (10u * 60u * 1000u)
static char     s_wx_cache_key[128];
static char     s_wx_cache_out[512];
static uint32_t s_wx_cache_at_ms;
static int      s_wx_cache_valid;

static uint32_t ws_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint32_t)ts.tv_sec * 1000u + (uint32_t)(ts.tv_nsec / 1000000));
}

static void ws_wx_cache_store(const char *location, const char *text)
{
    if (location == NULL || text == NULL || text[0] == '\0') {
        return;
    }

    strncpy(s_wx_cache_key, location, sizeof(s_wx_cache_key) - 1);
    strncpy(s_wx_cache_out, text, sizeof(s_wx_cache_out) - 1);
    s_wx_cache_at_ms = ws_now_ms();
    s_wx_cache_valid = 1;
}

static char s_serp_key[128]   = {0};
static char s_exa_key[128]    = {0};
static char s_news_key[128]   = {0};
static char s_tavily_key[128] = {0};

#define SEARCH_BUF_SIZE     (16 * 1024)
#define SEARCH_RESULT_COUNT 5

/* ──────────────────────────────────────────────────────────────
 * Helpers
 * ──────────────────────────────────────────────────────────────*/

static size_t url_encode(const char *src, char *dst, size_t dst_size)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t pos = 0;
    for (; *src && pos < dst_size - 3; src++) {
        unsigned char c = (unsigned char)*src;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            dst[pos++] = c;
        } else if (c == ' ') {
            dst[pos++] = '+';
        } else {
            dst[pos++] = '%';
            dst[pos++] = hex[c >> 4];
            dst[pos++] = hex[c & 0x0F];
        }
    }
    dst[pos] = '\0';
    return pos;
}

static int https_get_json(const char *host, const char *path,
                          const vela_header_t *hdrs,
                          char *buf, size_t buf_cap)
{
    return vela_https_request(host, "443", "GET", path, hdrs, NULL, 0, buf, buf_cap, NULL);
}

static int https_post_json(const char *host, const char *path,
                           const vela_header_t *hdrs,
                           const char *body,
                           char *buf, size_t buf_cap)
{
    return vela_https_request(host, "443", "POST", path,
                              hdrs, body, body ? strlen(body) : 0, buf, buf_cap, NULL);
}

/* ──────────────────────────────────────────────────────────────
 * Init
 * ──────────────────────────────────────────────────────────────*/

int tool_web_search_init(void)
{
    /* Compile-time secrets */
    if (AGENT_SECRET_SERP_KEY[0])   strncpy(s_serp_key,   AGENT_SECRET_SERP_KEY,   sizeof(s_serp_key)   - 1);
    if (AGENT_SECRET_EXA_KEY[0])    strncpy(s_exa_key,    AGENT_SECRET_EXA_KEY,    sizeof(s_exa_key)    - 1);
    if (AGENT_SECRET_NEWS_KEY[0])   strncpy(s_news_key,   AGENT_SECRET_NEWS_KEY,   sizeof(s_news_key)   - 1);
    if (AGENT_SECRET_TAVILY_KEY[0]) strncpy(s_tavily_key, AGENT_SECRET_TAVILY_KEY, sizeof(s_tavily_key) - 1);

    /* Runtime overrides from config store */
    char tmp[128] = {0};
    if (claw_config_get(AGENT_CFG_KEY_SERP_KEY, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_serp_key, tmp, sizeof(s_serp_key) - 1);
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_EXA_KEY, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_exa_key, tmp, sizeof(s_exa_key) - 1);
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_TAVILY_KEY, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_tavily_key, tmp, sizeof(s_tavily_key) - 1);
    memset(tmp, 0, sizeof(tmp));
    if (claw_config_get(AGENT_CFG_KEY_NEWS_KEY, tmp, sizeof(tmp)) == OK && tmp[0])
        strncpy(s_news_key, tmp, sizeof(s_news_key) - 1);

    if (s_tavily_key[0]) syslog(LOG_INFO, "[%s] Tavily key configured\n", TAG);
    if (s_serp_key[0])   syslog(LOG_INFO, "[%s] SerpAPI key configured\n", TAG);
    if (s_exa_key[0])    syslog(LOG_INFO, "[%s] Exa AI key configured\n", TAG);
    if (s_news_key[0])   syslog(LOG_INFO, "[%s] NewsAPI key configured\n", TAG);
    if (!s_tavily_key[0] && !s_serp_key[0])
        syslog(LOG_WARNING, "[%s] No search key. Use: set_tavily_key / set_search_key\n", TAG);

    return OK;
}

/* ──────────────────────────────────────────────────────────────
 * Backend 1: Tavily AI Search (primary — fast TLS)
 *   POST https://api.tavily.com/search
 *   Body: { "api_key":"...", "query":"...", "max_results":5 }
 *   Response: { "results": [{ "title","url","content" }] }
 * ──────────────────────────────────────────────────────────────*/

static int tavily_search(const char *query, char *output, size_t output_size)
{
    cJSON *body_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(body_obj, "api_key", s_tavily_key);
    cJSON_AddStringToObject(body_obj, "query", query);
    cJSON_AddNumberToObject(body_obj, "max_results", SEARCH_RESULT_COUNT);
    char *body_str = cJSON_PrintUnformatted(body_obj);
    cJSON_Delete(body_obj);
    if (!body_str) return ERROR;

    vela_header_t hdrs[] = {
        { "Content-Type", "application/json" },
        { NULL, NULL }
    };

    char *buf = calloc(1, SEARCH_BUF_SIZE);
    if (!buf) { free(body_str); return ERROR; }

    int status = https_post_json("api.tavily.com", "/search", hdrs, body_str, buf, SEARCH_BUF_SIZE);
    free(body_str);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] Tavily returned HTTP %d: %.200s\n", TAG, status, buf);
        free(buf);
        return ERROR;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ERROR;

    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No results found.");
        cJSON_Delete(root);
        return OK;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT) break;
        cJSON *title   = cJSON_GetObjectItem(item, "title");
        cJSON *url     = cJSON_GetObjectItem(item, "url");
        cJSON *content = cJSON_GetObjectItem(item, "content");
        off += snprintf(output + off, output_size - off,
            "%d. %s\n   %s\n   %s\n\n", idx + 1,
            title   && cJSON_IsString(title)   ? title->valuestring   : "(no title)",
            url     && cJSON_IsString(url)     ? url->valuestring     : "",
            content && cJSON_IsString(content) ? content->valuestring : "");
        if (off >= output_size - 1) break;
        idx++;
    }
    cJSON_Delete(root);
    return OK;
}

/* ──────────────────────────────────────────────────────────────
 * Backend 2: SerpAPI (Google)
 *   GET https://serpapi.com/search?engine=google&q=...&num=5&api_key=...
 *   Response: { "organic_results": [{ "title","link","snippet" }] }
 * ──────────────────────────────────────────────────────────────*/

static int serpapi_search(const char *query, char *output, size_t output_size)
{
    char encoded[256];
    url_encode(query, encoded, sizeof(encoded));

    char path[512];
    snprintf(path, sizeof(path),
             "/search?engine=google&q=%s&num=%d&api_key=%s",
             encoded, SEARCH_RESULT_COUNT, s_serp_key);

    char *buf = calloc(1, SEARCH_BUF_SIZE);
    if (!buf) return ERROR;

    vela_header_t hdrs[] = {
        { "Accept", "application/json" },
        { NULL, NULL }
    };

    int status = https_get_json("serpapi.com", path, hdrs, buf, SEARCH_BUF_SIZE);
    if (status != 200) {
        syslog(LOG_ERR, "[%s] SerpAPI returned %d\n", TAG, status);
        free(buf);
        return ERROR;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ERROR;

    cJSON *api_err = cJSON_GetObjectItem(root, "error");
    if (api_err && cJSON_IsString(api_err)) {
        snprintf(output, output_size, "SerpAPI error: %s", api_err->valuestring);
        cJSON_Delete(root);
        return ERROR;
    }

    cJSON *organic = cJSON_GetObjectItem(root, "organic_results");
    if (!organic || !cJSON_IsArray(organic) || cJSON_GetArraySize(organic) == 0) {
        snprintf(output, output_size, "No results found.");
        cJSON_Delete(root);
        return OK;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, organic) {
        if (idx >= SEARCH_RESULT_COUNT) break;
        cJSON *title   = cJSON_GetObjectItem(item, "title");
        cJSON *link    = cJSON_GetObjectItem(item, "link");
        cJSON *snippet = cJSON_GetObjectItem(item, "snippet");
        off += snprintf(output + off, output_size - off,
            "%d. %s\n   %s\n   %s\n\n", idx + 1,
            title   && cJSON_IsString(title)   ? title->valuestring   : "(no title)",
            link    && cJSON_IsString(link)    ? link->valuestring    : "",
            snippet && cJSON_IsString(snippet) ? snippet->valuestring : "");
        if (off >= output_size - 1) break;
        idx++;
    }
    cJSON_Delete(root);
    return OK;
}

/* ──────────────────────────────────────────────────────────────
 * Backend 3: Exa AI
 *   POST https://api.exa.ai/search
 *   Body: { "query":"...", "numResults":5, "useAutoprompt":true,
 *           "contents":{"text":true} }
 *   Auth: x-api-key header
 *   Response: { "results": [{ "title","url","text" }] }
 * ──────────────────────────────────────────────────────────────*/

static int exa_search(const char *query, char *output, size_t output_size)
{
    cJSON *body_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(body_obj, "query", query);
    cJSON_AddNumberToObject(body_obj, "numResults", SEARCH_RESULT_COUNT);
    cJSON_AddBoolToObject(body_obj, "useAutoprompt", 1);
    cJSON *contents = cJSON_CreateObject();
    cJSON_AddBoolToObject(contents, "text", 1);
    cJSON_AddItemToObject(body_obj, "contents", contents);
    char *body_str = cJSON_PrintUnformatted(body_obj);
    cJSON_Delete(body_obj);
    if (!body_str) return ERROR;

    vela_header_t hdrs[] = {
        { "Content-Type", "application/json" },
        { "x-api-key",    s_exa_key          },
        { NULL, NULL }
    };

    char *buf = calloc(1, SEARCH_BUF_SIZE);
    if (!buf) { free(body_str); return ERROR; }

    int status = https_post_json("api.exa.ai", "/search", hdrs, body_str, buf, SEARCH_BUF_SIZE);
    free(body_str);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] Exa returned HTTP %d: %.200s\n", TAG, status, buf);
        free(buf);
        return ERROR;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) return ERROR;

    cJSON *results = cJSON_GetObjectItem(root, "results");
    if (!results || !cJSON_IsArray(results) || cJSON_GetArraySize(results) == 0) {
        snprintf(output, output_size, "No results found.");
        cJSON_Delete(root);
        return OK;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, results) {
        if (idx >= SEARCH_RESULT_COUNT) break;
        cJSON *title   = cJSON_GetObjectItem(item, "title");
        cJSON *url     = cJSON_GetObjectItem(item, "url");
        cJSON *snippet = cJSON_GetObjectItem(item, "snippet");
        if (!snippet || !cJSON_IsString(snippet)) {
            cJSON *text_obj = cJSON_GetObjectItem(item, "text");
            if (text_obj && cJSON_IsString(text_obj)) snippet = text_obj;
        }
        off += snprintf(output + off, output_size - off,
            "%d. %s\n   %s\n   %s\n\n", idx + 1,
            title   && cJSON_IsString(title)   ? title->valuestring   : "(no title)",
            url     && cJSON_IsString(url)     ? url->valuestring     : "",
            snippet && cJSON_IsString(snippet) ? snippet->valuestring : "");
        if (off >= output_size - 1) break;
        idx++;
    }
    cJSON_Delete(root);
    return OK;
}

/* ──────────────────────────────────────────────────────────────
 * tool_web_search_execute  (Tavily primary → SerpAPI → Exa fallback)
 * ──────────────────────────────────────────────────────────────*/

int tool_web_search_execute(const char *input_json, char *output, size_t output_size)
{
    if (s_serp_key[0] == '\0' && s_tavily_key[0] == '\0' && s_exa_key[0] == '\0') {
        snprintf(output, output_size,
                 "Error: No search API key configured. Use CLI: set_search_key / set_tavily_key / set_exa_key");
        return ERROR;
    }

    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ERROR;
    }

    cJSON *query_item = cJSON_GetObjectItem(input, "query");
    if (!query_item || !cJSON_IsString(query_item) || query_item->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'query' field");
        return ERROR;
    }

    char query[256];
    strncpy(query, query_item->valuestring, sizeof(query) - 1);
    cJSON_Delete(input);

    syslog(LOG_INFO, "[%s] Web search: %s\n", TAG, query);

    if (s_tavily_key[0]) {
        int err = tavily_search(query, output, output_size);
        if (err == OK && output[0] && strncmp(output, "Error:", 6) != 0) {
            syslog(LOG_INFO, "[%s] Tavily OK: %d bytes\n", TAG, (int)strlen(output));
            return OK;
        }
        syslog(LOG_WARNING, "[%s] Tavily failed, trying SerpAPI fallback\n", TAG);
    }

    if (s_serp_key[0]) {
        int err = serpapi_search(query, output, output_size);
        if (err == OK && output[0] && strncmp(output, "Error:", 6) != 0) {
            syslog(LOG_INFO, "[%s] SerpAPI OK: %d bytes\n", TAG, (int)strlen(output));
            return OK;
        }
        syslog(LOG_WARNING, "[%s] SerpAPI failed, trying next fallback\n", TAG);
    }

    if (s_exa_key[0]) {
        int err = exa_search(query, output, output_size);
        if (err == OK) {
            syslog(LOG_INFO, "[%s] Exa AI OK: %d bytes\n", TAG, (int)strlen(output));
            return OK;
        }
        syslog(LOG_WARNING, "Exa AI failed, trying next fallback");
    }

    snprintf(output, output_size, "Error: All search backends failed.");
    return ERROR;
}

/* ──────────────────────────────────────────────────────────────
 * Backend 4: NewsAPI
 *   GET https://newsapi.org/v2/everything?q=...&pageSize=5&apiKey=...
 *   Response: { "articles": [{ "title","url","description","source":{"name"} }] }
 * ──────────────────────────────────────────────────────────────*/

int tool_news_search_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ERROR;
    }

    cJSON *query_item = cJSON_GetObjectItem(input, "query");
    if (!query_item || !cJSON_IsString(query_item) || query_item->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'query' field");
        return ERROR;
    }

    char query[256];
    strncpy(query, query_item->valuestring, sizeof(query) - 1);
    cJSON_Delete(input);

    syslog(LOG_INFO, "[%s] News search: %s\n", TAG, query);

    /* Primary: Tavily with topic=news — fast TLS, no routing issues */
    if (s_tavily_key[0]) {
        int err = tavily_search(query, output, output_size);
        if (err == OK && output[0] && strncmp(output, "Error:", 6) != 0) {
            syslog(LOG_INFO, "[%s] News via Tavily OK: %d bytes\n", TAG, (int)strlen(output));
            return OK;
        }
        syslog(LOG_WARNING, "[%s] Tavily news failed, trying NewsAPI\n", TAG);
    }

    /* Fallback: NewsAPI (may have routing issues on some networks) */
    if (s_news_key[0] == '\0') {
        snprintf(output, output_size,
                 "Error: No news search backend available. Set tavily key: set_tavily_key <KEY>");
        return ERROR;
    }

    char encoded[256];
    url_encode(query, encoded, sizeof(encoded));

    char path[512];
    snprintf(path, sizeof(path),
             "/v2/everything?q=%s&pageSize=%d&apiKey=%s",
             encoded, SEARCH_RESULT_COUNT, s_news_key);

    char *buf = calloc(1, SEARCH_BUF_SIZE);
    if (!buf) return ERROR;

    vela_header_t hdrs[] = {
        { "Accept", "application/json" },
        { NULL, NULL }
    };

    int status = https_get_json("newsapi.org", path, hdrs, buf, SEARCH_BUF_SIZE);
    if (status != 200) {
        syslog(LOG_ERR, "[%s] NewsAPI returned %d\n", TAG, status);
        free(buf);
        snprintf(output, output_size, "Error: NewsAPI request failed (%d)", status);
        return ERROR;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        snprintf(output, output_size, "Error: Failed to parse NewsAPI response");
        return ERROR;
    }

    cJSON *api_status = cJSON_GetObjectItem(root, "status");
    if (api_status && cJSON_IsString(api_status) &&
        strcmp(api_status->valuestring, "ok") != 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "message");
        snprintf(output, output_size, "NewsAPI error: %s",
                 msg && cJSON_IsString(msg) ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return ERROR;
    }

    cJSON *articles = cJSON_GetObjectItem(root, "articles");
    if (!articles || !cJSON_IsArray(articles) || cJSON_GetArraySize(articles) == 0) {
        snprintf(output, output_size, "No news articles found.");
        cJSON_Delete(root);
        return OK;
    }

    size_t off = 0;
    int idx = 0;
    cJSON *art;
    cJSON_ArrayForEach(art, articles) {
        if (idx >= SEARCH_RESULT_COUNT) break;
        cJSON *title    = cJSON_GetObjectItem(art, "title");
        cJSON *url      = cJSON_GetObjectItem(art, "url");
        cJSON *desc     = cJSON_GetObjectItem(art, "description");
        cJSON *src_obj  = cJSON_GetObjectItem(art, "source");
        cJSON *src_name = src_obj ? cJSON_GetObjectItem(src_obj, "name") : NULL;
        off += snprintf(output + off, output_size - off,
            "%d. [%s] %s\n   %s\n   %s\n\n", idx + 1,
            src_name && cJSON_IsString(src_name) ? src_name->valuestring : "?",
            title && cJSON_IsString(title) ? title->valuestring : "(no title)",
            url   && cJSON_IsString(url)   ? url->valuestring   : "",
            desc  && cJSON_IsString(desc)  ? desc->valuestring  : "");
        if (off >= output_size - 1) break;
        idx++;
    }
    cJSON_Delete(root);
    syslog(LOG_INFO, "[%s] NewsAPI OK: %d bytes\n", TAG, (int)strlen(output));
    return OK;
}

/* ──────────────────────────────────────────────────────────────
 * CLI key setters
 * ──────────────────────────────────────────────────────────────*/

int tool_web_search_set_serp_key(const char *key)
{
    claw_config_set(AGENT_CFG_KEY_SERP_KEY, key);
    strncpy(s_serp_key, key, sizeof(s_serp_key) - 1);
    syslog(LOG_INFO, "[%s] SerpAPI key saved\n", TAG);
    return OK;
}

int tool_web_search_set_exa_key(const char *key)
{
    claw_config_set(AGENT_CFG_KEY_EXA_KEY, key);
    strncpy(s_exa_key, key, sizeof(s_exa_key) - 1);
    syslog(LOG_INFO, "[%s] Exa AI key saved\n", TAG);
    return OK;
}

int tool_web_search_set_tavily_key(const char *key)
{
    claw_config_set(AGENT_CFG_KEY_TAVILY_KEY, key);
    strncpy(s_tavily_key, key, sizeof(s_tavily_key) - 1);
    syslog(LOG_INFO, "[%s] Tavily key saved\n", TAG);
    return OK;
}

int tool_web_search_set_news_key(const char *key)
{
    claw_config_set(AGENT_CFG_KEY_NEWS_KEY, key);
    strncpy(s_news_key, key, sizeof(s_news_key) - 1);
    syslog(LOG_INFO, "[%s] NewsAPI key saved\n", TAG);
    return OK;
}

/* ──────────────────────────────────────────────────────────────
 * get_weather tool
 *   Input JSON: { "location": "Shanghai" }
 *   Backends: Tavily → SerpAPI
 * ──────────────────────────────────────────────────────────────*/

/* ──────────────────────────────────────────────────────────────
 * Open-Meteo fallback（免 key）
 *
 * 天气页本来就走 api.open-meteo.com（免 key），而 get_weather 只走
 * Tavily/SerpAPI（都要 key）。没配 key 时语音问天气会返回英文错误并被
 * TTS 念出来（真机 2026-10-25 实测 "Error: Could not fetch weather..."）。
 * 这里补一条与天气页同源的兜底：固定北京坐标（手表没有定位）。
 * ──────────────────────────────────────────────────────────────*/
#define OM_WX_HOST     "api.open-meteo.com"
#define OM_WX_PORT     "443"
#define OM_WX_PATH     "/v1/forecast?latitude=39.9042&longitude=116.4074" \
                       "&current_weather=true"
#define OM_WX_RESP_CAP 2048

static const char *om_weather_desc(int code)
{
    if (code == 0)  return "晴";
    if (code <= 2)  return "多云";
    if (code == 3)  return "阴";
    if (code <= 48) return "有雾";
    if (code <= 57) return "毛毛雨";
    if (code <= 67) return "下雨";
    if (code <= 77) return "下雪";
    if (code <= 82) return "阵雨";
    if (code <= 86) return "阵雪";
    return "雷雨";
}

/* 只做"取数 + 解析"，把温度 / WMO 天气码 / 风速交出去。
 *
 * 为什么不和下面的格式化放在一起：后台预取线程（infra/weather_prefetch.c）
 * 与共享缓存要的是**数字**，不是一句中文。网络请求与 JSON 解析只留这一份，
 * 两个调用方各自决定怎么用（拼句子 / 写缓存）。
 *
 * resp 由调用方分配（OM_WX_RESP_CAP 字节）：这样 open_meteo_weather() 的
 * "内存不够"文案还在原来那一处，行为一点没变。 */
static int om_weather_fetch_raw(char *resp, size_t cap,
                                float *temp_c, int *code, float *wind_kmh)
{
    int status = vela_https_get(OM_WX_HOST, OM_WX_PORT, OM_WX_PATH,
                                resp, cap);
    if (status != 200) {
        syslog(LOG_WARNING, "[%s] Open-Meteo failed: %d\n", TAG, status);
        return ERROR;
    }

    cJSON *root = cJSON_Parse(resp);
    if (root == NULL) {
        return ERROR;
    }

    cJSON *cur  = cJSON_GetObjectItem(root, "current_weather");
    cJSON *jt   = cur ? cJSON_GetObjectItem(cur, "temperature") : NULL;
    cJSON *jc   = cur ? cJSON_GetObjectItem(cur, "weathercode") : NULL;
    cJSON *jw   = cur ? cJSON_GetObjectItem(cur, "windspeed") : NULL;
    if (!cJSON_IsNumber(jt) || !cJSON_IsNumber(jc)) {
        cJSON_Delete(root);
        return ERROR;
    }

    *temp_c   = (float)jt->valuedouble;
    *code     = (int)jc->valuedouble;
    *wind_kmh = cJSON_IsNumber(jw) ? (float)jw->valuedouble : 0.0f;
    cJSON_Delete(root);
    return OK;
}

static int open_meteo_weather(const char *location,
                              char *output, size_t output_size)
{
    float t  = 0.0f;
    int   wc = 0;
    float ws = 0.0f;
    char *resp = malloc(OM_WX_RESP_CAP);

    if (resp == NULL) {
        snprintf(output, output_size, "抱歉，内存不够，暂时查不了天气。");
        return ERROR;
    }

    if (om_weather_fetch_raw(resp, OM_WX_RESP_CAP, &t, &wc, &ws) != OK) {
        free(resp);
        return ERROR;
    }
    free(resp);

    /* 手表无定位：坐标固定北京；问别的城市时如实说明 */
    if (location != NULL && location[0] != '\0' &&
        strstr(location, "北京") == NULL &&
        strstr(location, "Beijing") == NULL &&
        strstr(location, "beijing") == NULL) {
        snprintf(output, output_size,
                 "手表没有定位，只能查北京：北京当前气温 %.1f 度，%s，"
                 "风速 %.1f 公里/小时。", t, om_weather_desc(wc), ws);
    } else {
        snprintf(output, output_size,
                 "北京当前气温 %.1f 度，%s，风速 %.1f 公里/小时。",
                 t, om_weather_desc(wc), ws);
    }
    syslog(LOG_INFO, "[%s] Weather via Open-Meteo OK (%.1f C, code %d)\n",
           TAG, t, wc);

    /* 共享缓存：与天气页、后台预取同一份（source="tool"）。
     * 只多写一份数据，不影响本工具的输出/自己的 10 分钟文本缓存；
     * 收益是语音问完天气以后，天气页进页面直接有数据可渲染。
     * 只有这一路会写：上面 Tavily/SerpAPI 返回的是自由文本，提不出温度/
     * 天气码，硬解析等于猜。 */
    weather_cache_update(t, wc, ws, "tool");
    return OK;
}

int tool_get_weather_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *input = cJSON_Parse(input_json);
    if (!input) {
        snprintf(output, output_size, "Error: Invalid input JSON");
        return ERROR;
    }

    cJSON *loc_item = cJSON_GetObjectItem(input, "location");
    if (!loc_item || !cJSON_IsString(loc_item) || loc_item->valuestring[0] == '\0') {
        cJSON_Delete(input);
        snprintf(output, output_size, "Error: Missing 'location' field");
        return ERROR;
    }

    char location[256];
    strncpy(location, loc_item->valuestring, sizeof(location) - 1);
    cJSON_Delete(input);

    syslog(LOG_INFO, "[%s] Weather query: %s\n", TAG, location);

    /* 10 分钟缓存命中: 直接回上次的中文结果, 不联网 */
    if (s_wx_cache_valid && s_wx_cache_out[0] != '\0'
        && strcmp(location, s_wx_cache_key) == 0
        && (uint32_t)(ws_now_ms() - s_wx_cache_at_ms) < WS_WX_TTL_MS) {
        snprintf(output, output_size, "%s", s_wx_cache_out);
        syslog(LOG_INFO, "[%s] Weather cache hit: %s\n", TAG, location);
        return OK;
    }

    char search_query[300];
    snprintf(search_query, sizeof(search_query), "%s weather today", location);

    if (s_tavily_key[0]) {
        int err = tavily_search(search_query, output, output_size);
        if (err == OK && output[0] && strncmp(output, "Error:", 6) != 0) {
            syslog(LOG_INFO, "[%s] Weather via Tavily OK\n", TAG);
            ws_wx_cache_store(location, output);
            return OK;
        }
        syslog(LOG_WARNING, "[%s] Tavily weather failed, trying SerpAPI\n", TAG);
    }

    if (s_serp_key[0]) {
        int err = serpapi_search(search_query, output, output_size);
        if (err == OK && output[0] && strncmp(output, "Error:", 6) != 0) {
            syslog(LOG_INFO, "[%s] Weather via SerpAPI OK\n", TAG);
            ws_wx_cache_store(location, output);
            return OK;
        }
    }

    /* 没配搜索 key（或两个后端都失败）时走免 key 的 Open-Meteo 兜底。
     * 中文输出：它会被直接播报，英文错误用户听不懂。 */
    if (open_meteo_weather(location, output, output_size) == OK) {
        ws_wx_cache_store(location, output);
        return OK;
    }

    snprintf(output, output_size,
             "抱歉，现在查不到%s的天气（网络或数据源不可用）。", location);
    return ERROR;
}

/* Legacy alias */
int tool_web_search_set_key(const char *api_key)
{
    return tool_web_search_set_serp_key(api_key);
}

/* ──────────────────────────────────────────────────────────────
 * 共享缓存入口：给后台预取线程（infra/weather_prefetch.c）用
 *
 * 只拉北京这一份 Open-Meteo 数据，写进 infra/weather_cache；成功/失败都
 * 记录 —— last_ok 就是给这一步用的（"有没有成功数据" 和 "最近一次成功
 * 没有"是两件事：天气页缓存过期时照样能拿旧数据显示）。
 *
 * 参数 desc_out 只是给日志用的中文描述（"晴"/"多云"…），预取线程不需要
 * 一整句话，所以这里只回一个词，不拼句子、不写 output 缓冲区。
 *
 * 失败不重试：天气是低速数据，30 分钟后再来，别和语音/LLM 的 TLS 抢 IOB。
 * ────────────────────────────────────────────────────────────── */
int tool_weather_fetch_now(char *desc_out, size_t cap)
{
    char *resp;
    float t  = 0.0f;
    int   wc = 0;
    float ws = 0.0f;

    if (desc_out != NULL && cap > 0) {
        desc_out[0] = '\0';
    }

    resp = malloc(OM_WX_RESP_CAP);
    if (resp == NULL) {
        weather_cache_mark_fail();
        return ERROR;
    }

    if (om_weather_fetch_raw(resp, OM_WX_RESP_CAP, &t, &wc, &ws) != OK) {
        free(resp);
        weather_cache_mark_fail();
        return ERROR;
    }
    free(resp);

    weather_cache_update(t, wc, ws, "prefetch");

    if (desc_out != NULL && cap > 0) {
        snprintf(desc_out, cap, "%s", om_weather_desc(wc));
    }

    return OK;
}

