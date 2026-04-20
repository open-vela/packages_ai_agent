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

#include "channels/feishu_internal.h"
#include "infra/config_store.h"
#include "infra/http_proxy.h"
#include "infra/url_parse.h"
#include "infra/vela_tls.h"
#include "cJSON.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char* TAG = "feishu_http";

/* ── Feishu API helpers ──────────────────────────────────────── */

int feishu_https_post(const char* path, const vela_header_t* extra_headers,
    const char* json_body, char* resp_buf, size_t resp_cap)
{
    if (!http_proxy_is_enabled()) {
        return vela_https_post_json("open.feishu.cn", "443", path, extra_headers,
            json_body, resp_buf, resp_cap);
    }

    /* ── Via HTTP CONNECT proxy ─────────────────────────────── */
    proxy_conn_t* conn = proxy_conn_open("open.feishu.cn", 443, 30000);

    if (!conn) {
        syslog(LOG_ERR, "[%s] proxy_conn_open failed for %s\n", TAG, path);
        return -1;
    }

    int body_len = json_body ? (int)strlen(json_body) : 0;
    char* header = malloc(1024);

    if (!header) {
        proxy_conn_close(conn);
        return -1;
    }

    int hlen = snprintf(header, 1024,
        "POST %s HTTP/1.1\r\n"
        "Host: open.feishu.cn\r\n"
        "Content-Type: application/json\r\n",
        path);

    if (extra_headers) {
        for (const vela_header_t* h = extra_headers; h->name; h++) {
            hlen += snprintf(header + hlen, 1024 - (size_t)hlen, "%s: %s\r\n",
                h->name, h->value);
        }
    }
    hlen += snprintf(header + hlen, 1024 - (size_t)hlen,
        "Content-Length: %d\r\nConnection: close\r\n\r\n", body_len);

    if (proxy_conn_write(conn, header, hlen) < 0 ||
        (body_len > 0 && proxy_conn_write(conn, json_body, body_len) < 0)) {
        free(header);
        proxy_conn_close(conn);
        return -1;
    }
    free(header);

    /* Read full response */
    int total = 0;
    int cap = (int)resp_cap - 1;

    while (total < cap) {
        int n = proxy_conn_read(conn, resp_buf + total, cap - total, 30000);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    resp_buf[total] = '\0';
    proxy_conn_close(conn);

    /* Parse HTTP status */
    int status = 0;

    if (total > 5 && strncmp(resp_buf, "HTTP/", 5) == 0) {
        const char* sp = strchr(resp_buf, ' ');
        if (sp) {
            status = atoi(sp + 1);
        }
    }

    /* Strip HTTP headers, keep body */
    char* body_start = strstr(resp_buf, "\r\n\r\n");

    if (body_start) {
        body_start += 4;
        size_t blen = (size_t)(total - (int)(body_start - resp_buf));
        memmove(resp_buf, body_start, blen);
        resp_buf[blen] = '\0';
    }

    return status;
}

int feishu_https_request(const char* method, const char* path,
    const vela_header_t* headers, const char* body,
    size_t body_len, char* resp_buf, size_t resp_cap,
    size_t* out_body_len)
{
    if (!http_proxy_is_enabled()) {
        return vela_https_request("open.feishu.cn", "443", method, path, headers,
            body, body_len, resp_buf, resp_cap, out_body_len);
    }

    proxy_conn_t* conn = proxy_conn_open("open.feishu.cn", 443, 30000);

    if (!conn) {
        syslog(LOG_ERR, "[%s] proxy_conn_open failed for %s %s\n", TAG, method,
            path);
        return -1;
    }

    char* header = malloc(1024);

    if (!header) {
        proxy_conn_close(conn);
        return -1;
    }

    int hlen = snprintf(header, 1024,
        "%s %s HTTP/1.1\r\n"
        "Host: open.feishu.cn\r\n",
        method, path);

    if (headers) {
        for (const vela_header_t* h = headers; h->name; h++) {
            hlen += snprintf(header + hlen, 1024 - (size_t)hlen, "%s: %s\r\n",
                h->name, h->value);
        }
    }
    if (body && body_len > 0) {
        hlen += snprintf(header + hlen, 1024 - (size_t)hlen,
            "Content-Length: %d\r\n", (int)body_len);
    }
    hlen += snprintf(header + hlen, 1024 - (size_t)hlen,
        "Connection: close\r\n\r\n");

    if (proxy_conn_write(conn, header, hlen) < 0) {
        free(header);
        proxy_conn_close(conn);
        return -1;
    }
    free(header);

    if (body && body_len > 0) {
        if (proxy_conn_write(conn, body, (int)body_len) < 0) {
            proxy_conn_close(conn);
            return -1;
        }
    }

    /* Read full response */
    int total = 0;
    int cap = (int)resp_cap - 1;

    while (total < cap) {
        int n = proxy_conn_read(conn, resp_buf + total, cap - total, 30000);
        if (n <= 0) {
            break;
        }
        total += n;
    }
    resp_buf[total] = '\0';
    proxy_conn_close(conn);

    /* Parse HTTP status */
    int status = 0;

    if (total > 5 && strncmp(resp_buf, "HTTP/", 5) == 0) {
        const char* sp = strchr(resp_buf, ' ');
        if (sp) {
            status = atoi(sp + 1);
        }
    }

    /* Strip HTTP headers, keep body */
    char* body_start = strstr(resp_buf, "\r\n\r\n");

    if (body_start) {
        body_start += 4;
        size_t blen = (size_t)(total - (int)(body_start - resp_buf));
        memmove(resp_buf, body_start, blen);
        resp_buf[blen] = '\0';
        if (out_body_len) {
            *out_body_len = blen;
        }
    } else {
        if (out_body_len) {
            *out_body_len = (size_t)total;
        }
    }

    return status;
}

int feishu_get_app_token(void)
{
    char body[256];

    snprintf(body, sizeof(body), "{\"app_id\":\"%s\",\"app_secret\":\"%s\"}",
        s_app_id, s_app_secret);

    char* resp = malloc(4096);

    if (!resp) {
        return ERROR;
    }

    int status = feishu_https_post(
        "/open-apis/auth/v3/tenant_access_token/internal", NULL, body, resp, 4096);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] tenant_access_token HTTP %d: %.200s\n", TAG, status,
            resp);
        free(resp);
        return ERROR;
    }

    cJSON* root = cJSON_Parse(resp);

    free(resp);
    if (!root) {
        return ERROR;
    }

    cJSON* code = cJSON_GetObjectItem(root, "code");

    if (!cJSON_IsNumber(code) || (int)code->valuedouble != 0) {
        syslog(LOG_ERR, "[%s] tenant_access_token code=%d\n", TAG,
            cJSON_IsNumber(code) ? (int)code->valuedouble : -1);
        cJSON_Delete(root);
        return ERROR;
    }

    cJSON* tok = cJSON_GetObjectItem(root, "tenant_access_token");
    cJSON* expire = cJSON_GetObjectItem(root, "expire");

    if (!cJSON_IsString(tok)) {
        cJSON_Delete(root);
        return ERROR;
    }

    strncpy(s_access_token, tok->valuestring, sizeof(s_access_token) - 1);

    if (cJSON_IsNumber(expire)) {
        struct timespec ts = { 0 };
        clock_gettime(CLOCK_REALTIME, &ts);
        s_token_expire_ts = ts.tv_sec + (int64_t)expire->valuedouble - 300;
    }

    cJSON_Delete(root);
    syslog(LOG_INFO, "[%s] tenant_access_token refreshed\n", TAG);

    /* Fetch bot's own open_id once (needed to filter self-mentions) */
    if (s_bot_open_id[0] == '\0') {
        char* bi_resp = malloc(2048);
        if (bi_resp) {
            char auth_val[520];
            snprintf(auth_val, sizeof(auth_val), "Bearer %s", s_access_token);
            vela_header_t bi_hdrs[] = {
                { "Authorization", auth_val }, { NULL, NULL }
            };
            size_t bi_len = 0;
            int bi_status = feishu_https_request("GET", "/open-apis/bot/v3/info",
                bi_hdrs, NULL, 0, bi_resp, 2048, &bi_len);
            if (bi_status == 200) {
                cJSON* bi_root = cJSON_Parse(bi_resp);
                if (bi_root) {
                    cJSON* bi_bot = cJSON_GetObjectItem(bi_root, "bot");
                    if (cJSON_IsObject(bi_bot)) {
                        cJSON* oid = cJSON_GetObjectItem(bi_bot, "open_id");
                        if (cJSON_IsString(oid) && oid->valuestring[0]) {
                            strncpy(s_bot_open_id, oid->valuestring,
                                sizeof(s_bot_open_id) - 1);
                            syslog(LOG_INFO, "[%s] Bot open_id=%s\n", TAG,
                                s_bot_open_id);
                        }
                    }
                    cJSON_Delete(bi_root);
                }
            }
            free(bi_resp);
        }
    }

    return OK;
}

bool feishu_token_expired(void)
{
    if (s_access_token[0] == '\0') {
        return true;
    }
    struct timespec ts = { 0 };
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec >= s_token_expire_ts;
}

int feishu_create_connection(char* ws_host, size_t host_cap,
    char* ws_path, size_t path_cap)
{
    char* resp = malloc(4096);

    if (!resp) {
        return ERROR;
    }

    char body[256];

    snprintf(body, sizeof(body), "{\"AppID\":\"%s\",\"AppSecret\":\"%s\"}",
        s_app_id, s_app_secret);

    vela_header_t headers[] = { { "locale", "zh" }, { NULL, NULL } };

    int status = feishu_https_post("/callback/ws/endpoint", headers, body,
        resp, 4096);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] ws/endpoint HTTP %d\n", TAG, status);
        free(resp);
        return ERROR;
    }

    cJSON* root = cJSON_Parse(resp);

    free(resp);
    if (!root) {
        syslog(LOG_ERR, "[%s] ws/endpoint bad JSON\n", TAG);
        return ERROR;
    }

    cJSON* code_item = cJSON_GetObjectItem(root, "code");

    if (!cJSON_IsNumber(code_item) || code_item->valueint != 0) {
        cJSON* msg = cJSON_GetObjectItem(root, "msg");
        syslog(LOG_ERR, "[%s] ws/endpoint code=%lld msg=%s\n", TAG,
            code_item ? code_item->valueint : (cJSON_int)-1,
            (msg && cJSON_IsString(msg)) ? msg->valuestring : "?");
        cJSON_Delete(root);
        return ERROR;
    }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    cJSON* url = data ? cJSON_GetObjectItem(data, "URL") : NULL;

    if (!cJSON_IsString(url) || !url->valuestring[0]) {
        syslog(LOG_ERR, "[%s] ws/endpoint: no URL in response\n", TAG);
        cJSON_Delete(root);
        return ERROR;
    }

    const char* ws_url = url->valuestring;

    syslog(LOG_INFO, "[%s] Got WS URL: %s\n", TAG, ws_url);

    parsed_url_t parsed;
    if (url_parse(ws_url, &parsed) != 0) {
        syslog(LOG_ERR, "[%s] Invalid WS URL: %s\n", TAG, ws_url);
        cJSON_Delete(root);
        return ERROR;
    }

    strncpy(ws_host, parsed.host, host_cap - 1);
    ws_host[host_cap - 1] = '\0';
    strncpy(ws_path, parsed.path, path_cap - 1);
    ws_path[path_cap - 1] = '\0';

    cJSON_Delete(root);

    syslog(LOG_INFO, "[%s] WS endpoint: host=%s path=%.80s...\n", TAG, ws_host,
        ws_path);
    return OK;
}
