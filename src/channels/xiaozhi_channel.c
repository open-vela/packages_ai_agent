/*
 * Copyright (C) 2025 Xiaomi Corporation
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
 * xiaozhi_channel.c — XiaoZhi WebSocket protocol channel.
 *
 * Connects to XiaoZhi Server via WSS. Single connection handles both
 * JSON control messages (text frames) and Opus audio (binary frames).
 * No MQTT, no UDP, no AES — WSS provides TLS encryption.
 *
 * Flow: OTA → get token → WSS connect → hello → session ready
 */

#include "channels/xiaozhi_channel.h"
#include "channels/xiaozhi_state.h"
#include "infra/ws_client.h"
#include "infra/vela_tls.h"
#include "infra/url_parse.h"
#include "agent_config.h"
#include "agent_compat.h"
#include "infra/config_store.h"
#include "core/message_bus.h"
#include "cJSON.h"

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "xiaozhi";

/* ── State machine ────────────────────────────────────────── */

static const uint8_t s_allowed[XZ_STATE_MAX][XZ_STATE_MAX] = {
    /* DISC CONN WCON CHOP */
    { 0, 1, 0, 0 }, /* Disconnected */
    { 1, 0, 1, 0 }, /* Connecting */
    { 1, 0, 0, 1 }, /* Connected */
    { 1, 0, 1, 0 }, /* ChannelOpened */
};

static xiaozhi_state_t s_state = XZ_STATE_DISCONNECTED;
static pthread_mutex_t s_state_lock = PTHREAD_MUTEX_INITIALIZER;

static int set_state(xiaozhi_state_t ns)
{
    int ret = 0;
    pthread_mutex_lock(&s_state_lock);
    xiaozhi_state_t old = s_state;
    if (ns >= XZ_STATE_MAX || !s_allowed[old][ns]) {
        syslog(LOG_WARNING, "[%s] bad transition: %s→%s\n",
            TAG, xiaozhi_state_name(old), xiaozhi_state_name(ns));
        ret = -EINVAL;
    } else {
        s_state = ns;
        syslog(LOG_INFO, "[%s] state: %s→%s\n",
            TAG, xiaozhi_state_name(old), xiaozhi_state_name(ns));
    }
    pthread_mutex_unlock(&s_state_lock);
    return ret;
}

static void force_disconnect(void)
{
    pthread_mutex_lock(&s_state_lock);
    s_state = XZ_STATE_DISCONNECTED;
    pthread_mutex_unlock(&s_state_lock);
}

/* ── Channel context ──────────────────────────────────────── */

static volatile bool s_running;
static sem_t s_ws_exit_sem;
static bool s_ws_thread_running;
static bool s_sem_initialized;

/* Config */
static char s_ota_url[256];   /* e.g. "https://host/xiaozhi/ota/" */
static char s_device_id[32];  /* e.g. "AA:BB:CC:DD:EE:01" */
static char s_client_id[64];  /* UUID */

/* OTA results */
static char s_ws_url[256];
static char s_ws_token[256];
static char s_ws_host[128];
static char s_ws_port[8];
static char s_ws_path[128];

/* Session */
static char s_session_id[64];
static ws_client_t s_ws;
static pthread_mutex_t s_ws_send_lock = PTHREAD_MUTEX_INITIALIZER;

/* Thread-safe WebSocket send (CLI thread + WS thread both call this) */
static int xz_ws_send(uint8_t opcode, const void* data, size_t len)
{
    pthread_mutex_lock(&s_ws_send_lock);
    int ret = ws_client_send(&s_ws, opcode, data, len);
    pthread_mutex_unlock(&s_ws_send_lock);
    return ret;
}

/* ── OTA: get WebSocket URL + token ───────────────────────── */

static int fetch_ota_config(void)
{
    /* Parse OTA URL */
    parsed_url_t ota;
    if (url_parse(s_ota_url, &ota) != 0) {
        syslog(LOG_ERR, "[%s] Invalid OTA URL: %s\n", TAG, s_ota_url);
        return -EINVAL;
    }

    /* Build OTA request body */
    cJSON* body = cJSON_CreateObject();
    if (!body) return -ENOMEM;

    cJSON* app = cJSON_AddObjectToObject(body, "application");
    if (app) {
        cJSON_AddStringToObject(app, "version", "1.0.1");
        cJSON_AddStringToObject(app, "elf_sha256", "1");
    }
    cJSON* board = cJSON_AddObjectToObject(body, "board");
    if (board)
        cJSON_AddStringToObject(board, "mac", s_device_id);

    char* json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!json) return -ENOMEM;

    /* POST to OTA endpoint */
    vela_header_t hdrs[] = {
        { "Content-Type", "application/json" },
        { "Device-Id", s_device_id },
        { "Client-Id", s_client_id },
        { NULL, NULL },
    };

    char* resp = malloc(4096);
    if (!resp) { free(json); return -ENOMEM; }

    syslog(LOG_INFO, "[%s] OTA POST to %s:%s%s (%d bytes)\n",
        TAG, ota.host, ota.port, ota.path, (int)strlen(json));

    int status = vela_https_request(ota.host, ota.port, "POST", ota.path,
        hdrs, json, strlen(json), resp, 4096, NULL);
    free(json);

    syslog(LOG_INFO, "[%s] OTA response status=%d\n", TAG, status);

    if (status < 200 || status >= 300) {
        syslog(LOG_ERR, "[%s] OTA failed: status=%d\n", TAG, status);
        free(resp);
        return -EIO;
    }

    syslog(LOG_INFO, "[%s] OTA body: %.64s...\n", TAG, resp);

    /* Parse response */
    cJSON* root = cJSON_Parse(resp);
    free(resp);
    if (!root) return -EINVAL;

    cJSON* ws = cJSON_GetObjectItem(root, "websocket");
    if (!cJSON_IsObject(ws)) {
        syslog(LOG_ERR, "[%s] OTA: no websocket field\n", TAG);
        cJSON_Delete(root);
        return -EINVAL;
    }

    cJSON* url = cJSON_GetObjectItem(ws, "url");
    cJSON* token = cJSON_GetObjectItem(ws, "token");

    if (cJSON_IsString(url)) {
        strncpy(s_ws_url, url->valuestring, sizeof(s_ws_url) - 1);
        s_ws_url[sizeof(s_ws_url) - 1] = '\0';
    }
    if (cJSON_IsString(token)) {
        strncpy(s_ws_token, token->valuestring, sizeof(s_ws_token) - 1);
        s_ws_token[sizeof(s_ws_token) - 1] = '\0';
    }

    cJSON_Delete(root);

    /* Parse WSS URL */
    parsed_url_t ws_parsed;
    if (url_parse(s_ws_url, &ws_parsed) != 0) {
        syslog(LOG_ERR, "[%s] Invalid WS URL: %s\n", TAG, s_ws_url);
        return -EINVAL;
    }

    memcpy(s_ws_host, ws_parsed.host, sizeof(s_ws_host));
    memcpy(s_ws_port, ws_parsed.port, sizeof(s_ws_port));
    memcpy(s_ws_path, ws_parsed.path, sizeof(s_ws_path));

    syslog(LOG_INFO, "[%s] OTA OK: ws=%s token=%.20s...\n",
        TAG, s_ws_url, s_ws_token);
    return 0;
}

/* ── Message handlers ─────────────────────────────────────── */

static void handle_hello(cJSON* root)
{
    cJSON* sid = cJSON_GetObjectItem(root, "session_id");
    if (cJSON_IsString(sid)) {
        strncpy(s_session_id, sid->valuestring, sizeof(s_session_id) - 1);
        s_session_id[sizeof(s_session_id) - 1] = '\0';
    }
    syslog(LOG_INFO, "[%s] hello OK: session=%s\n", TAG, s_session_id);
    set_state(XZ_STATE_CHANNEL_OPENED);
}

static void handle_stt(cJSON* root)
{
    cJSON* text = cJSON_GetObjectItem(root, "text");
    if (!cJSON_IsString(text) || !text->valuestring || !text->valuestring[0])
        return;
    syslog(LOG_INFO, "[%s] STT: %s\n", TAG, text->valuestring);

    agent_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, "xiaozhi", sizeof(msg.channel) - 1);
    msg.channel[sizeof(msg.channel) - 1] = '\0';
    strncpy(msg.chat_id, s_session_id, sizeof(msg.chat_id) - 1);
    msg.chat_id[sizeof(msg.chat_id) - 1] = '\0';
    msg.content = strdup(text->valuestring);
    if (msg.content) {
        if (message_bus_push_inbound(&msg) != OK)
            free(msg.content);
    }
}

static void handle_tts(cJSON* root)
{
    cJSON* state = cJSON_GetObjectItem(root, "state");
    if (cJSON_IsString(state))
        syslog(LOG_INFO, "[%s] TTS: %s\n", TAG, state->valuestring);

    /* Log TTS text content */
    cJSON* text = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(text) && text->valuestring[0])
        syslog(LOG_INFO, "[%s] TTS text: %s\n", TAG, text->valuestring);
}

static void handle_llm(cJSON* root)
{
    cJSON* emotion = cJSON_GetObjectItem(root, "emotion");
    if (cJSON_IsString(emotion))
        syslog(LOG_INFO, "[%s] LLM emotion: %s\n", TAG, emotion->valuestring);
}

static void handle_mcp(cJSON* root)
{
    cJSON* payload = cJSON_GetObjectItem(root, "payload");
    if (!cJSON_IsObject(payload))
        return;

    cJSON* method = cJSON_GetObjectItem(payload, "method");
    cJSON* id = cJSON_GetObjectItem(payload, "id");
    if (!cJSON_IsString(method) || !cJSON_IsNumber(id))
        return;

    const char* m = method->valuestring;
    int mid = id->valueint;

    cJSON* reply = cJSON_CreateObject();
    if (!reply) return;
    cJSON_AddStringToObject(reply, "session_id", s_session_id);
    cJSON_AddStringToObject(reply, "type", "mcp");

    cJSON* rp = cJSON_AddObjectToObject(reply, "payload");
    if (!rp) { cJSON_Delete(reply); return; }
    cJSON_AddStringToObject(rp, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(rp, "id", mid);

    if (strcmp(m, "initialize") == 0) {
        cJSON* result = cJSON_AddObjectToObject(rp, "result");
        if (result) {
            cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
            cJSON* caps = cJSON_AddObjectToObject(result, "capabilities");
            if (caps) {
                cJSON* tools = cJSON_AddObjectToObject(caps, "tools");
                if (tools) cJSON_AddBoolToObject(tools, "listChanged", true);
            }
            cJSON* info = cJSON_AddObjectToObject(result, "serverInfo");
            if (info) {
                cJSON_AddStringToObject(info, "name", "ai_agent");
                cJSON_AddStringToObject(info, "version", "1.0");
            }
        }
        syslog(LOG_INFO, "[%s] MCP: replied initialize\n", TAG);
    } else if (strcmp(m, "tools/list") == 0) {
        cJSON* result = cJSON_AddObjectToObject(rp, "result");
        if (result)
            cJSON_AddArrayToObject(result, "tools");
        syslog(LOG_INFO, "[%s] MCP: replied tools/list\n", TAG);
    } else {
        syslog(LOG_INFO, "[%s] MCP: unhandled method=%s\n", TAG, m);
        cJSON_Delete(reply);
        return;
    }

    char* json = cJSON_PrintUnformatted(reply);
    cJSON_Delete(reply);
    if (json) {
        xz_ws_send(WS_OPCODE_TEXT, json, strlen(json));
        free(json);
    }
}

static void dispatch_json(const char* data, size_t len)
{
    cJSON* root = cJSON_Parse(data);
    if (!root) {
        syslog(LOG_WARNING, "[%s] invalid JSON: %.64s\n", TAG, data);
        return;
    }

    cJSON* type = cJSON_GetObjectItem(root, "type");
    if (!cJSON_IsString(type)) {
        cJSON_Delete(root);
        return;
    }

    const char* t = type->valuestring;
    if (strcmp(t, "hello") == 0) handle_hello(root);
    else if (strcmp(t, "stt") == 0) handle_stt(root);
    else if (strcmp(t, "tts") == 0) handle_tts(root);
    else if (strcmp(t, "llm") == 0) handle_llm(root);
    else if (strcmp(t, "mcp") == 0) handle_mcp(root);
    else if (strcmp(t, "goodbye") == 0) {
        syslog(LOG_INFO, "[%s] server goodbye\n", TAG);
        s_running = false;
    } else {
        syslog(LOG_DEBUG, "[%s] msg type=%s\n", TAG, t);
    }

    cJSON_Delete(root);
    (void)len;
}

/* ── WebSocket thread (with auto-reconnect) ───────────────── */

#define XZ_RECONNECT_BASE_SEC 2
#define XZ_RECONNECT_MAX_SEC  60

static void* ws_task(void* arg)
{
    (void)arg;
    syslog(LOG_INFO, "[%s] WS thread started\n", TAG);

    int backoff_sec = 0;

    while (s_running) {
        /* Backoff before reconnect (skip on first attempt) */
        if (backoff_sec > 0) {
            syslog(LOG_INFO, "[%s] reconnecting in %ds...\n",
                TAG, backoff_sec);
            for (int i = 0; i < backoff_sec && s_running; i++)
                sleep(1);
            if (!s_running) break;
        }

        /* Step 1: Fetch OTA config */
        if (fetch_ota_config() != 0) {
            syslog(LOG_ERR, "[%s] OTA fetch failed\n", TAG);
            goto retry;
        }

        if (!s_ws_url[0]) {
            syslog(LOG_ERR, "[%s] No WebSocket URL from OTA\n", TAG);
            goto retry;
        }

        /* Step 2: Init + connect WebSocket */
        if (ws_client_init(&s_ws) != 0) {
            syslog(LOG_ERR, "[%s] ws_client_init failed\n", TAG);
            goto retry;
        }

        /* Wire up send lock so PONG inside ws_client_recv is serialized
         * with sends from CLI thread (xiaozhi_channel_send_text). */
        s_ws.send_lock = &s_ws_send_lock;

        {
            char auth_val[300];
            snprintf(auth_val, sizeof(auth_val), "Bearer %s", s_ws_token);

            ws_header_t headers[] = {
                { "Authorization", auth_val },
                { "Protocol-Version", "1" },
                { "Device-Id", s_device_id },
                { "Client-Id", s_client_id },
                { NULL, NULL },
            };

            set_state(XZ_STATE_CONNECTING);

            if (ws_client_connect(&s_ws, s_ws_host, s_ws_port,
                    s_ws_path, headers) != 0) {
                syslog(LOG_ERR, "[%s] WSS connect failed\n", TAG);
                force_disconnect();
                goto retry;
            }
        }

        set_state(XZ_STATE_CONNECTED);
        backoff_sec = 0; /* reset on successful connect */

        /* Send hello */
        {
            cJSON* hello = cJSON_CreateObject();
            if (hello) {
                cJSON_AddStringToObject(hello, "type", "hello");
                cJSON_AddNumberToObject(hello, "version", 1);
                cJSON_AddStringToObject(hello, "transport", "websocket");
                cJSON* feat = cJSON_AddObjectToObject(hello, "features");
                if (feat) cJSON_AddBoolToObject(feat, "mcp", true);
                cJSON* audio = cJSON_AddObjectToObject(hello, "audio_params");
                if (audio) {
                    cJSON_AddStringToObject(audio, "format", "opus");
                    cJSON_AddNumberToObject(audio, "sample_rate", 16000);
                    cJSON_AddNumberToObject(audio, "channels", 1);
                    cJSON_AddNumberToObject(audio, "frame_duration", 60);
                }
                char* json = cJSON_PrintUnformatted(hello);
                cJSON_Delete(hello);
                if (json) {
                    xz_ws_send(WS_OPCODE_TEXT, json, strlen(json));
                    syslog(LOG_INFO, "[%s] hello sent\n", TAG);
                    free(json);
                }
            }
        }

        /* Receive loop */
        {
            char buf[4096];
            while (s_running) {
                uint8_t opcode = 0;
                int n = ws_client_recv(&s_ws, &opcode, buf, sizeof(buf) - 1);

                if (n < 0) {
                    if (s_running)
                        syslog(LOG_WARNING, "[%s] WS recv error, will reconnect\n", TAG);
                    break; /* break recv loop → reconnect */
                }
                if (n == 0)
                    continue;

                if (opcode == WS_OPCODE_TEXT) {
                    buf[n] = '\0';
                    dispatch_json(buf, (size_t)n);
                } else if (opcode == WS_OPCODE_BINARY) {
                    syslog(LOG_DEBUG, "[%s] binary frame: %d bytes\n", TAG, n);
                }
            }
        }

        ws_client_close(&s_ws);
        force_disconnect();
        continue; /* loop back to reconnect */

retry:
        ws_client_close(&s_ws);
        force_disconnect();
        if (backoff_sec < XZ_RECONNECT_BASE_SEC)
            backoff_sec = XZ_RECONNECT_BASE_SEC;
        else if (backoff_sec < XZ_RECONNECT_MAX_SEC)
            backoff_sec *= 2;
        if (backoff_sec > XZ_RECONNECT_MAX_SEC)
            backoff_sec = XZ_RECONNECT_MAX_SEC;
    }

    syslog(LOG_INFO, "[%s] WS thread exiting\n", TAG);
    pthread_mutex_lock(&s_state_lock);
    s_ws_thread_running = false;
    pthread_mutex_unlock(&s_state_lock);
    sem_post(&s_ws_exit_sem);
    return NULL;
}

/* ── Public API ───────────────────────────────────────────── */

int xiaozhi_channel_init(void)
{
    memset(s_ota_url, 0, sizeof(s_ota_url));
    memset(s_device_id, 0, sizeof(s_device_id));
    memset(s_client_id, 0, sizeof(s_client_id));
    memset(s_session_id, 0, sizeof(s_session_id));
    s_ws_thread_running = false;
    s_sem_initialized = false;

    claw_config_get(AGENT_CFG_KEY_XIAOZHI_ENDPOINT,
        s_ota_url, sizeof(s_ota_url));
    claw_config_get(AGENT_CFG_KEY_XIAOZHI_CLIENT_ID,
        s_client_id, sizeof(s_client_id));

    /* Device ID: from config or generate from node ID */
    claw_config_get(AGENT_CFG_KEY_XIAOZHI_DEVICE_ID,
        s_device_id, sizeof(s_device_id));
    if (!s_device_id[0] || strchr(s_device_id, ':') == NULL) {
        /* Generate a stable MAC-like ID from node ID hash */
        uint8_t hash[6];
        const char* nid = CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_ID;
        size_t nid_len = strlen(nid);
        if (nid_len == 0) {
            nid = "agent-01";
            nid_len = strlen(nid);
        }
        for (int i = 0; i < 6; i++) {
            hash[i] = (uint8_t)(nid[i % nid_len] ^ (i * 37));
        }
        hash[0] |= 0x02; /* locally administered bit */
        snprintf(s_device_id, sizeof(s_device_id),
            "%02X:%02X:%02X:%02X:%02X:%02X",
            hash[0], hash[1], hash[2], hash[3], hash[4], hash[5]);
        syslog(LOG_INFO, "[%s] generated device_id: %s\n", TAG, s_device_id);
    }

    /* Client ID: from config or generate UUID */
    if (!s_client_id[0]) {
        uint8_t uuid_bytes[16];
        if (agent_secure_random(uuid_bytes, sizeof(uuid_bytes)) == 0) {
            uuid_bytes[6] = (uuid_bytes[6] & 0x0F) | 0x40; /* version 4 */
            uuid_bytes[8] = (uuid_bytes[8] & 0x3F) | 0x80; /* variant 1 */
            snprintf(s_client_id, sizeof(s_client_id),
                "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                uuid_bytes[0], uuid_bytes[1], uuid_bytes[2], uuid_bytes[3],
                uuid_bytes[4], uuid_bytes[5], uuid_bytes[6], uuid_bytes[7],
                uuid_bytes[8], uuid_bytes[9], uuid_bytes[10], uuid_bytes[11],
                uuid_bytes[12], uuid_bytes[13], uuid_bytes[14], uuid_bytes[15]);
        } else {
            snprintf(s_client_id, sizeof(s_client_id),
                "agent-%s-%08x", CONFIG_EXAMPLES_AI_AGENT_VELA_NODE_ID, (unsigned)rand());
        }
    }

    syslog(LOG_INFO, "[%s] init (ota=%s)\n",
        TAG, s_ota_url[0] ? s_ota_url : "(not set)");
    return 0;
}

int xiaozhi_channel_start(void)
{
    if (!s_ota_url[0]) {
        syslog(LOG_INFO, "[%s] No endpoint configured, skipping\n", TAG);
        return 0;
    }

    /* Idempotent guard — prevent double start (under lock) */
    pthread_mutex_lock(&s_state_lock);
    if (s_ws_thread_running) {
        pthread_mutex_unlock(&s_state_lock);
        syslog(LOG_WARNING, "[%s] already running, ignoring start()\n", TAG);
        return 0;
    }

    /* Mark as running before releasing lock to prevent double-start race */
    s_ws_thread_running = true;
    s_running = true;
    pthread_mutex_unlock(&s_state_lock);

    /* OTA fetch + WSS connect happen in the ws_task thread
     * to avoid blocking network_watch_task */
    if (sem_init(&s_ws_exit_sem, 0, 0) != 0) {
        syslog(LOG_ERR, "[%s] sem_init failed\n", TAG);
        s_running = false;
        pthread_mutex_lock(&s_state_lock);
        s_ws_thread_running = false;
        pthread_mutex_unlock(&s_state_lock);
        return -ENOMEM;
    }
    s_sem_initialized = true;

    int ret = agent_task_create(ws_task, "xz_ws",
        16384, NULL, CONFIG_EXAMPLES_AI_AGENT_VELA_PRIORITY);
    if (ret != OK) {
        syslog(LOG_ERR, "[%s] Failed to create WS thread\n", TAG);
        sem_destroy(&s_ws_exit_sem);
        s_sem_initialized = false;
        s_running = false;
        pthread_mutex_lock(&s_state_lock);
        s_ws_thread_running = false;
        pthread_mutex_unlock(&s_state_lock);
        return -EIO;
    }

    syslog(LOG_INFO, "[%s] started (OTA+WSS in background)\n", TAG);
    return 0;
}

int xiaozhi_channel_send_text(const char* text)
{
    if (!text)
        return -1;

    /* Snapshot connection state under lock */
    pthread_mutex_lock(&s_state_lock);
    bool ready = s_running && s_ws.connected && s_session_id[0];
    char session[sizeof(s_session_id)];
    if (ready)
        memcpy(session, s_session_id, sizeof(session));
    pthread_mutex_unlock(&s_state_lock);

    if (!ready)
        return -1;

    /* Send listen start */
    cJSON* listen = cJSON_CreateObject();
    if (!listen) return -ENOMEM;
    cJSON_AddStringToObject(listen, "session_id", session);
    cJSON_AddStringToObject(listen, "type", "listen");
    cJSON_AddStringToObject(listen, "state", "start");
    cJSON_AddStringToObject(listen, "mode", "manual");
    cJSON_AddStringToObject(listen, "text", text);
    char* json = cJSON_PrintUnformatted(listen);
    cJSON_Delete(listen);
    if (!json) return -ENOMEM;

    int ret = xz_ws_send(WS_OPCODE_TEXT, json, strlen(json));
    syslog(LOG_INFO, "[%s] sent listen text: %s\n", TAG, text);
    free(json);

    if (ret < 0) return -EIO;

    /* Send listen stop */
    cJSON* stop = cJSON_CreateObject();
    if (!stop) return -ENOMEM;
    cJSON_AddStringToObject(stop, "session_id", session);
    cJSON_AddStringToObject(stop, "type", "listen");
    cJSON_AddStringToObject(stop, "state", "stop");
    json = cJSON_PrintUnformatted(stop);
    cJSON_Delete(stop);
    if (json) {
        xz_ws_send(WS_OPCODE_TEXT, json, strlen(json));
        free(json);
    }

    return 0;
}

int xiaozhi_channel_send_reply(const char* text)
{
    if (!text)
        return -1;

    /* Snapshot connection state under lock */
    pthread_mutex_lock(&s_state_lock);
    bool ready = s_running && s_ws.connected && s_session_id[0];
    char session[sizeof(s_session_id)];
    if (ready)
        memcpy(session, s_session_id, sizeof(session));
    pthread_mutex_unlock(&s_state_lock);

    if (!ready)
        return -1;

    /* Send TTS start with reply text */
    cJSON* tts = cJSON_CreateObject();
    if (!tts) return -ENOMEM;
    cJSON_AddStringToObject(tts, "session_id", session);
    cJSON_AddStringToObject(tts, "type", "tts");
    cJSON_AddStringToObject(tts, "state", "start");
    cJSON_AddStringToObject(tts, "text", text);
    char* json = cJSON_PrintUnformatted(tts);
    cJSON_Delete(tts);
    if (!json) return -ENOMEM;

    int ret = xz_ws_send(WS_OPCODE_TEXT, json, strlen(json));
    syslog(LOG_INFO, "[%s] sent reply: %.64s\n", TAG, text);
    free(json);
    json = NULL;

    if (ret < 0) return -EIO;

    /* Send TTS stop */
    cJSON* stop = cJSON_CreateObject();
    if (!stop) return -ENOMEM;
    cJSON_AddStringToObject(stop, "session_id", session);
    cJSON_AddStringToObject(stop, "type", "tts");
    cJSON_AddStringToObject(stop, "state", "stop");
    json = cJSON_PrintUnformatted(stop);
    cJSON_Delete(stop);
    if (json) {
        xz_ws_send(WS_OPCODE_TEXT, json, strlen(json));
        free(json);
    }

    return 0;
}

void xiaozhi_channel_stop(void)
{
    bool was_running = s_running;
    s_running = false;

    /* Shutdown socket to unblock recv — don't close/free, thread owns that */
    ws_client_shutdown_fd(&s_ws);

    pthread_mutex_lock(&s_state_lock);
    bool need_join = s_ws_thread_running;
    bool need_sem_destroy = s_sem_initialized;
    pthread_mutex_unlock(&s_state_lock);

    if (need_join) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 3;
        if (sem_timedwait(&s_ws_exit_sem, &ts) != 0)
            syslog(LOG_ERR, "[%s] WS thread did not exit in time\n", TAG);

        pthread_mutex_lock(&s_state_lock);
        s_ws_thread_running = false;
        pthread_mutex_unlock(&s_state_lock);
    }

    /* Destroy semaphore after thread has exited and posted */
    if (need_sem_destroy) {
        sem_destroy(&s_ws_exit_sem);
        pthread_mutex_lock(&s_state_lock);
        s_sem_initialized = false;
        pthread_mutex_unlock(&s_state_lock);
    }

    force_disconnect();
    syslog(LOG_INFO, "[%s] stopped (was_running=%d)\n", TAG, was_running);
}
