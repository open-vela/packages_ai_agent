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

/**
 * weixin_channel.c — WeChat (Weixin) channel for AI Agent
 *
 * Direct connection to Tencent iLink Bot API (ilinkai.weixin.qq.com).
 * No intermediate gateway needed.
 *
 * Protocol: https://github.com/nicepkg/weixin-bot/docs/protocol-spec.md
 *
 * Long-polling flow:
 *   1. POST /ilink/bot/getupdates  (hold ≤35s)
 *   2. Server blocks until message arrives or timeout
 *   3. Parse msgs[] array, push each to message_bus inbound queue
 *   4. Update cursor and repeat
 *
 * Outbound:
 *   POST /ilink/bot/sendmessage  (with context_token)
 *
 * Login:
 *   GET  /ilink/bot/get_bot_qrcode?bot_type=3
 *   GET  /ilink/bot/get_qrcode_status?qrcode=<qrc>
 */

#include "channels/weixin_channel.h"
#include "core/message_bus.h"
#include "cJSON.h"
#include "infra/config_store.h"
#include "infra/url_parse.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"
#include "agent_config.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* ── Constants ────────────────────────────────────────────────── */

static const char* TAG = "weixin";

#define WEIXIN_DEFAULT_HOST "ilinkai.weixin.qq.com"
#define WEIXIN_DEFAULT_PORT "443"
#define WEIXIN_RESP_BUF_SIZE (32 * 1024)
#define WEIXIN_MAX_MSGS_PER_POLL 16
#define WEIXIN_RECONNECT_DELAY_S 5
#define WEIXIN_POLL_TIMEOUT_MS 35000
#define WEIXIN_TOKEN_MAX 256
#define WEIXIN_HOST_MAX 128
#define WEIXIN_PORT_MAX 8
#define WEIXIN_CURSOR_MAX 512
#define WEIXIN_CHANNEL_VERSION "2.0.0"

/* ── Module state ─────────────────────────────────────────────── */

static char s_host[WEIXIN_HOST_MAX] = WEIXIN_DEFAULT_HOST;
static char s_port[WEIXIN_PORT_MAX] = WEIXIN_DEFAULT_PORT;
static char s_token[WEIXIN_TOKEN_MAX] = "";

static pthread_t s_poll_thread;
static volatile int s_running = 0;

/* ── Message dedup ring (16 slots on message_id) ─────────────── */

#define DEDUP_RING_SIZE 16
static char s_dedup_ring[DEDUP_RING_SIZE][24];
static int s_dedup_idx = 0;

static int dedup_check_and_record(const char* msg_id_str)
{
    if (!msg_id_str || msg_id_str[0] == '\0')
        return 0;
    for (int i = 0; i < DEDUP_RING_SIZE; i++) {
        if (strcmp(s_dedup_ring[i], msg_id_str) == 0)
            return 1;
    }
    strncpy(s_dedup_ring[s_dedup_idx], msg_id_str,
        sizeof(s_dedup_ring[0]) - 1);
    s_dedup_ring[s_dedup_idx][sizeof(s_dedup_ring[0]) - 1] = '\0';
    s_dedup_idx = (s_dedup_idx + 1) % DEDUP_RING_SIZE;
    return 0;
}

/* ── Base64 encode (RFC 4648, no line wrapping) ──────────────── */

static const char B64_CHARS[]
    = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_encode(const uint8_t* src, size_t src_len,
    char* dst, size_t dst_cap)
{
    size_t out = 0;
    for (size_t i = 0; i < src_len; i += 3) {
        if (out + 4 >= dst_cap)
            return -1;
        uint32_t v = (uint32_t)src[i] << 16;
        if (i + 1 < src_len)
            v |= (uint32_t)src[i + 1] << 8;
        if (i + 2 < src_len)
            v |= (uint32_t)src[i + 2];
        dst[out++] = B64_CHARS[(v >> 18) & 0x3f];
        dst[out++] = B64_CHARS[(v >> 12) & 0x3f];
        dst[out++] = (i + 1 < src_len) ? B64_CHARS[(v >> 6) & 0x3f] : '=';
        dst[out++] = (i + 2 < src_len) ? B64_CHARS[v & 0x3f] : '=';
    }
    dst[out] = '\0';
    return (int)out;
}

/* ── X-WECHAT-UIN: base64(decimal_string_of_random_uint32) ───── */

static void weixin_gen_uin(char* uin_buf, size_t uin_cap)
{
    uint32_t val = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    char dec[16];
    snprintf(dec, sizeof(dec), "%" PRIu32, val);
    b64_encode((const uint8_t*)dec, strlen(dec), uin_buf, uin_cap);
}

/* ── Build common request headers ────────────────────────────── */

static void weixin_build_headers(vela_header_t* hdrs, size_t n,
    char* auth_buf, size_t auth_cap,
    char* uin_buf, size_t uin_cap)
{
    if (n < 5)
        return;

    snprintf(auth_buf, auth_cap, "Bearer %s", s_token);
    weixin_gen_uin(uin_buf, uin_cap);

    hdrs[0].name = "Content-Type";
    hdrs[0].value = "application/json";
    hdrs[1].name = "AuthorizationType";
    hdrs[1].value = "ilink_bot_token";
    hdrs[2].name = "Authorization";
    hdrs[2].value = auth_buf;
    hdrs[3].name = "X-WECHAT-UIN";
    hdrs[3].value = uin_buf;
    hdrs[4].name = NULL;
    hdrs[4].value = NULL;
}

/* ── HTTPS POST helper ───────────────────────────────────────── */

static int weixin_post(const char* path,
    const char* json_body,
    char* resp_buf, size_t resp_cap)
{
    char auth_buf[WEIXIN_TOKEN_MAX + 16];
    char uin_buf[32];
    vela_header_t hdrs[5];

    weixin_build_headers(hdrs, 5, auth_buf, sizeof(auth_buf),
        uin_buf, sizeof(uin_buf));

    return vela_https_post_json(
        s_host, s_port, path,
        hdrs, json_body,
        resp_buf, resp_cap);
}

/* ── Add base_info to cJSON request object ───────────────────── */

static void weixin_add_base_info(cJSON* req)
{
    cJSON* bi = cJSON_CreateObject();
    if (bi) {
        cJSON_AddStringToObject(bi, "channel_version", WEIXIN_CHANNEL_VERSION);
        cJSON_AddItemToObject(req, "base_info", bi);
    }
}

/* ── Parse and dispatch a single WeixinMessage ───────────────── */

static void weixin_dispatch_message(cJSON* msg_obj)
{
    cJSON* from_j = cJSON_GetObjectItem(msg_obj, "from_user_id");
    cJSON* type_j = cJSON_GetObjectItem(msg_obj, "message_type");
    cJSON* state_j = cJSON_GetObjectItem(msg_obj, "message_state");
    cJSON* items_j = cJSON_GetObjectItem(msg_obj, "item_list");
    cJSON* ctx_j = cJSON_GetObjectItem(msg_obj, "context_token");
    cJSON* id_j = cJSON_GetObjectItem(msg_obj, "message_id");

    int msg_type = cJSON_IsNumber(type_j) ? (int)type_j->valuedouble : 0;
    int msg_state = cJSON_IsNumber(state_j) ? (int)state_j->valuedouble : 2;

    const char* from_raw = cJSON_IsString(from_j) ? from_j->valuestring : "?";

    /* Extract message_id as string — avoid snprintf("%.0f") which
     * triggers NuttX float formatting and corrupts stack canary
     * on real hardware (lib_stackchk.c:57 crash). */
    char msg_id_str[24] = "";
    if (cJSON_IsNumber(id_j)) {
        char* tmp = cJSON_PrintUnformatted(id_j);
        if (tmp) {
            strncpy(msg_id_str, tmp, sizeof(msg_id_str) - 1);
            free(tmp);
        }
    }

    syslog(LOG_DEBUG, "[%s] dispatch: type=%d state=%d from=%.30s id=%s\n",
        TAG, msg_type, msg_state, from_raw, msg_id_str);

    if (msg_type != 1 || msg_state != 2) {
        syslog(LOG_DEBUG, "[%s] skip: type=%d state=%d (want 1/2)\n",
            TAG, msg_type, msg_state);
        return;
    }

    if (dedup_check_and_record(msg_id_str)) {
        syslog(LOG_DEBUG, "[%s] skip: dedup id=%s\n", TAG, msg_id_str);
        return;
    }

    const char* from_user_id = cJSON_IsString(from_j) ? from_j->valuestring : "";
    const char* ctx_token = cJSON_IsString(ctx_j) ? ctx_j->valuestring : "";

    /* Heap-allocate to avoid blowing the 12 KB poll-task stack */
#define TEXT_BUF_SIZE 4096
    char* text_buf = calloc(1, TEXT_BUF_SIZE);
    if (!text_buf) {
        syslog(LOG_ERR, "[%s] OOM: text_buf\n", TAG);
        return;
    }

    if (cJSON_IsArray(items_j)) {
        int n = cJSON_GetArraySize(items_j);
        for (int i = 0; i < n; i++) {
            cJSON* item = cJSON_GetArrayItem(items_j, i);
            cJSON* itype = cJSON_GetObjectItem(item, "type");
            cJSON* titem = cJSON_GetObjectItem(item, "text_item");
            int itype_v = cJSON_IsNumber(itype)
                ? (int)itype->valuedouble
                : 0;

            if (itype_v == 1 && cJSON_IsObject(titem)) {
                cJSON* txt = cJSON_GetObjectItem(titem, "text");
                if (cJSON_IsString(txt) && txt->valuestring[0]) {
                    size_t cur = strlen(text_buf);
                    size_t rem = TEXT_BUF_SIZE - cur - 1;
                    if (cur > 0 && rem > 1) {
                        text_buf[cur] = ' ';
                        cur++;
                        rem--;
                    }
                    strncpy(text_buf + cur, txt->valuestring, rem);
                    text_buf[TEXT_BUF_SIZE - 1] = '\0';
                }
            }
        }
    }

    if (text_buf[0] == '\0') {
        free(text_buf);
        return;
    }

    syslog(LOG_INFO, "[%s] inbound from=%s len=%zu\n",
        TAG, from_user_id, strlen(text_buf));

    char chat_id[128];
    snprintf(chat_id, sizeof(chat_id), "%s|%s",
        from_user_id, ctx_token);

    agent_msg_t bus_msg;
    memset(&bus_msg, 0, sizeof(bus_msg));
    strncpy(bus_msg.channel, AGENT_CHAN_WEIXIN,
        sizeof(bus_msg.channel) - 1);
    strncpy(bus_msg.chat_id, chat_id, sizeof(bus_msg.chat_id) - 1);
    bus_msg.content = strdup(text_buf);
    free(text_buf);
    if (!bus_msg.content) {
        syslog(LOG_ERR, "[%s] OOM in weixin_dispatch_message\n", TAG);
        return;
    }

    if (message_bus_push_inbound(&bus_msg) != OK) {
        syslog(LOG_WARNING, "[%s] message_bus_push_inbound failed\n",
            TAG);
        free(bus_msg.content);
    }
}

/* ── Long-poll task ───────────────────────────────────────────── */

static void* weixin_poll_task(void* arg)
{
    (void)arg;
    syslog(LOG_INFO, "[%s] Long-poll started (%s:%s)\n",
        TAG, s_host, s_port);

    char cursor[WEIXIN_CURSOR_MAX] = "";
    int err_backoff = WEIXIN_RECONNECT_DELAY_S;
    char* resp_buf = malloc(WEIXIN_RESP_BUF_SIZE);
    if (!resp_buf) {
        syslog(LOG_ERR, "[%s] OOM: response buffer\n", TAG);
        return NULL;
    }

    while (s_running) {
        if (s_token[0] == '\0') {
            syslog(LOG_WARNING, "[%s] No token, waiting...\n", TAG);
            sleep(WEIXIN_RECONNECT_DELAY_S);
            continue;
        }

        cJSON* req = cJSON_CreateObject();
        if (!req) {
            sleep(1);
            continue;
        }
        cJSON_AddStringToObject(req, "get_updates_buf", cursor);
        weixin_add_base_info(req);
        char* req_str = cJSON_PrintUnformatted(req);
        cJSON_Delete(req);
        if (!req_str) {
            sleep(1);
            continue;
        }

        memset(resp_buf, 0, WEIXIN_RESP_BUF_SIZE);
        int http_rc = weixin_post("/ilink/bot/getupdates",
            req_str, resp_buf, WEIXIN_RESP_BUF_SIZE);
        free(req_str);

        if (http_rc < 0 || http_rc != 200) {
            syslog(LOG_WARNING, "[%s] getupdates HTTP %d\n",
                TAG, http_rc);
            sleep(WEIXIN_RECONNECT_DELAY_S);
            continue;
        }

        cJSON* resp = cJSON_Parse(resp_buf);
        if (!resp) {
            syslog(LOG_WARNING, "[%s] JSON parse failed\n", TAG);
            sleep(1);
            continue;
        }

        cJSON* ret_j = cJSON_GetObjectItem(resp, "ret");
        cJSON* msgs_j = cJSON_GetObjectItem(resp, "msgs");
        int ret;
        if (cJSON_IsNumber(ret_j)) {
            ret = (int)ret_j->valuedouble;
        } else if (cJSON_IsArray(msgs_j)) {
            /* iLink API: success response has no "ret" field,
             * just "msgs" array directly. Treat as ret=0. */
            ret = 0;
        } else {
            ret = -1;
        }

        if (ret == -14) {
            syslog(LOG_WARNING, "[%s] Session expired (ret=-14), "
                                "need re-login\n",
                TAG);
            cursor[0] = '\0';
            s_token[0] = '\0';
            claw_config_set("weixin.token", "");
            cJSON_Delete(resp);
            sleep(WEIXIN_RECONNECT_DELAY_S);
            continue;
        }

        if (ret != 0) {
            syslog(LOG_WARNING, "[%s] getupdates ret=%d, "
                                "backoff %ds\n",
                TAG, ret, err_backoff);
            cJSON_Delete(resp);
            sleep(err_backoff);
            if (err_backoff < 60)
                err_backoff *= 2;
            continue;
        }

        err_backoff = WEIXIN_RECONNECT_DELAY_S; /* reset on success */

        cJSON* next_cursor_j = cJSON_GetObjectItem(resp, "get_updates_buf");
        if (cJSON_IsString(next_cursor_j)
            && next_cursor_j->valuestring[0]) {
            strncpy(cursor, next_cursor_j->valuestring,
                sizeof(cursor) - 1);
            cursor[sizeof(cursor) - 1] = '\0';
        }

        if (cJSON_IsArray(msgs_j)) {
            int n = cJSON_GetArraySize(msgs_j);
            syslog(n > 0 ? LOG_INFO : LOG_DEBUG,
                "[%s] poll: ret=%d msgs=%d cursor=%.20s\n",
                TAG, ret, n, cursor);
            for (int i = 0; i < n && i < WEIXIN_MAX_MSGS_PER_POLL; i++) {
                cJSON* m = cJSON_GetArrayItem(msgs_j, i);
                if (cJSON_IsObject(m))
                    weixin_dispatch_message(m);
            }
        }

        cJSON_Delete(resp);
    }

    free(resp_buf);
    resp_buf = NULL;
    syslog(LOG_INFO, "[%s] Poll task exited\n", TAG);
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────── */

int weixin_channel_init(void)
{
    char host_cfg[WEIXIN_HOST_MAX] = "";
    char port_cfg[WEIXIN_PORT_MAX] = "";
    char token_cfg[WEIXIN_TOKEN_MAX] = "";

    claw_config_get("weixin.host", host_cfg, sizeof(host_cfg));
    claw_config_get("weixin.port", port_cfg, sizeof(port_cfg));
    claw_config_get("weixin.token", token_cfg, sizeof(token_cfg));

    if (host_cfg[0]) {
        strncpy(s_host, host_cfg, sizeof(s_host) - 1);
        s_host[sizeof(s_host) - 1] = '\0';
    }
    if (port_cfg[0]) {
        strncpy(s_port, port_cfg, sizeof(s_port) - 1);
        s_port[sizeof(s_port) - 1] = '\0';
    }
    if (token_cfg[0]) {
        strncpy(s_token, token_cfg, sizeof(s_token) - 1);
        s_token[sizeof(s_token) - 1] = '\0';
    }

    srand((unsigned int)time(NULL));

    syslog(LOG_INFO, "[%s] init: host=%s port=%s token=%s\n",
        TAG, s_host, s_port, s_token[0] ? "set" : "(none)");
    return 0;
}

int weixin_channel_start(void)
{
    if (s_running) {
        syslog(LOG_WARNING, "[%s] Already running\n", TAG);
        return 0;
    }
    s_running = 1;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, AGENT_WEIXIN_STACK);

    int rc = pthread_create(&s_poll_thread, &attr,
        weixin_poll_task, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        syslog(LOG_ERR, "[%s] pthread_create failed: %d\n", TAG, rc);
        s_running = 0;
        return -1;
    }

    syslog(LOG_INFO, "[%s] Poll task started\n", TAG);
    return 0;
}

int weixin_channel_send(const char* to_user_id,
    const char* context_token,
    const char* text)
{
    if (!to_user_id || !text)
        return -1;
    if (s_token[0] == '\0') {
        syslog(LOG_WARNING, "[%s] send: no token\n", TAG);
        return -1;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON* msg_obj = cJSON_CreateObject();
    cJSON* items = cJSON_CreateArray();
    cJSON* item = cJSON_CreateObject();
    cJSON* txt_item = cJSON_CreateObject();

    if (!root || !msg_obj || !items || !item || !txt_item) {
        syslog(LOG_ERR, "[%s] send: cJSON OOM\n", TAG);
        cJSON_Delete(root);
        cJSON_Delete(msg_obj);
        cJSON_Delete(items);
        cJSON_Delete(item);
        cJSON_Delete(txt_item);
        return -1;
    }

    cJSON_AddStringToObject(msg_obj, "from_user_id", "");
    cJSON_AddStringToObject(msg_obj, "to_user_id", to_user_id);

    /* Generate unique client_id */
    char client_id[64];
    snprintf(client_id, sizeof(client_id),
        "agent-%ld-%04x",
        (long)time(NULL), (unsigned)(rand() & 0xffff));
    cJSON_AddStringToObject(msg_obj, "client_id", client_id);

    cJSON_AddNumberToObject(msg_obj, "message_type", 2); /* BOT */
    cJSON_AddNumberToObject(msg_obj, "message_state", 2); /* FINISH */

    if (context_token && context_token[0])
        cJSON_AddStringToObject(msg_obj, "context_token", context_token);

    cJSON_AddStringToObject(txt_item, "text", text);
    cJSON_AddNumberToObject(item, "type", 1);
    cJSON_AddItemToObject(item, "text_item", txt_item);
    cJSON_AddItemToArray(items, item);
    cJSON_AddItemToObject(msg_obj, "item_list", items);
    cJSON_AddItemToObject(root, "msg", msg_obj);
    weixin_add_base_info(root);

    char* req_str = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!req_str) {
        syslog(LOG_ERR, "[%s] send: cJSON OOM\n", TAG);
        return -1;
    }

    char resp_buf[1024] = "";
    int http_rc = weixin_post("/ilink/bot/sendmessage",
        req_str, resp_buf, sizeof(resp_buf));
    free(req_str);

    if (http_rc != 200) {
        syslog(LOG_WARNING, "[%s] sendmessage HTTP %d to %s\n",
            TAG, http_rc, to_user_id);
        return -1;
    }

    /* Check iLink business error in response body */
    if (resp_buf[0]) {
        cJSON* resp = cJSON_Parse(resp_buf);
        if (resp) {
            cJSON* ec = cJSON_GetObjectItem(resp, "errcode");
            if (ec && cJSON_IsNumber(ec) && ec->valueint != 0) {
                cJSON* em = cJSON_GetObjectItem(resp, "errmsg");
                syslog(LOG_WARNING,
                    "[%s] sendmessage errcode=%lld errmsg=%s\n",
                    TAG, (long long)ec->valueint,
                    (em && em->valuestring) ? em->valuestring : "?");
                cJSON_Delete(resp);
                return -1;
            }
            cJSON_Delete(resp);
        }
    }

    syslog(LOG_INFO, "[%s] Sent to %s\n", TAG, to_user_id);
    return 0;
}

void weixin_channel_stop(void)
{
    if (!s_running)
        return;
    s_running = 0;
    pthread_join(s_poll_thread, NULL);
    syslog(LOG_INFO, "[%s] Channel stopped\n", TAG);
}

int weixin_channel_set_token(const char* token, unsigned int uin)
{
    (void)uin; /* UIN is now generated per-request */
    if (!token || !token[0])
        return -1;
    strncpy(s_token, token, sizeof(s_token) - 1);
    s_token[sizeof(s_token) - 1] = '\0';

    claw_config_set("weixin.token", s_token);

    syslog(LOG_INFO, "[%s] Token updated\n", TAG);
    return 0;
}

int weixin_channel_login(char* qr_url, size_t qr_cap,
    char* qrcode_id, size_t qrc_cap)
{
    if (!qr_url || !qrcode_id)
        return -1;

    char resp_buf[2048] = "";
    int rc = vela_https_get(s_host, s_port,
        "/ilink/bot/get_bot_qrcode?bot_type=3",
        resp_buf, sizeof(resp_buf));
    if (rc != 200) {
        syslog(LOG_WARNING, "[%s] get_bot_qrcode HTTP %d\n", TAG, rc);
        return -1;
    }

    cJSON* resp = cJSON_Parse(resp_buf);
    if (!resp) {
        syslog(LOG_WARNING, "[%s] get_bot_qrcode JSON fail\n", TAG);
        return -1;
    }

    cJSON* qrc_j = cJSON_GetObjectItem(resp, "qrcode");
    cJSON* url_j = cJSON_GetObjectItem(resp, "qrcode_img_content");

    if (!cJSON_IsString(qrc_j) || !cJSON_IsString(url_j)) {
        syslog(LOG_WARNING, "[%s] get_bot_qrcode missing fields\n", TAG);
        cJSON_Delete(resp);
        return -1;
    }

    strncpy(qrcode_id, qrc_j->valuestring, qrc_cap - 1);
    qrcode_id[qrc_cap - 1] = '\0';
    strncpy(qr_url, url_j->valuestring, qr_cap - 1);
    qr_url[qr_cap - 1] = '\0';

    cJSON_Delete(resp);
    syslog(LOG_INFO, "[%s] QR code ready: %s\n", TAG, qr_url);
    return 0;
}

int weixin_channel_poll_login(const char* qrcode_id)
{
    if (!qrcode_id || !qrcode_id[0])
        return -1;

    char path[256];
    int n = snprintf(path, sizeof(path),
        "/ilink/bot/get_qrcode_status?qrcode=%s", qrcode_id);
    if (n < 0 || (size_t)n >= sizeof(path))
        return -1;

    char resp_buf[2048] = "";
    int rc = vela_https_get(s_host, s_port, path,
        resp_buf, sizeof(resp_buf));
    if (rc != 200)
        return -2; /* HTTP error */

    cJSON* resp = cJSON_Parse(resp_buf);
    if (!resp)
        return -1;

    cJSON* status_j = cJSON_GetObjectItem(resp, "status");
    const char* status = cJSON_IsString(status_j)
        ? status_j->valuestring
        : "";

    int result;
    if (strcmp(status, "confirmed") == 0) {
        /* Extract and save credentials */
        cJSON* tok_j = cJSON_GetObjectItem(resp, "bot_token");
        cJSON* base_j = cJSON_GetObjectItem(resp, "baseurl");

        if (cJSON_IsString(tok_j)) {
            strncpy(s_token, tok_j->valuestring,
                sizeof(s_token) - 1);
            s_token[sizeof(s_token) - 1] = '\0';
            claw_config_set("weixin.token", s_token);
        }

        if (cJSON_IsString(base_j) && base_j->valuestring[0]) {
            /* Parse host from baseurl (https://host) */
            parsed_url_t parsed;
            if (url_parse(base_j->valuestring, &parsed) == 0) {
                strncpy(s_host, parsed.host, sizeof(s_host) - 1);
                s_host[sizeof(s_host) - 1] = '\0';
            } else {
                /* Fallback: use raw value */
                strncpy(s_host, base_j->valuestring, sizeof(s_host) - 1);
                s_host[sizeof(s_host) - 1] = '\0';
            }
            claw_config_set("weixin.host", s_host);
        }

        syslog(LOG_INFO, "[%s] Login confirmed, token saved\n", TAG);
        result = 1; /* confirmed */
    } else if (strcmp(status, "scaned") == 0) {
        result = 2; /* scanned, waiting confirm */
    } else if (strcmp(status, "expired") == 0) {
        result = -3; /* QR expired */
    } else {
        result = 0; /* waiting */
    }

    cJSON_Delete(resp);
    return result;
}
