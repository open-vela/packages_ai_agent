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

#include "node/node_client.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "cJSON.h"
#include "core/message_bus.h"
#include "infra/config_store.h"
#include "tools/tool_registry.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "mbedtls/base64.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

static const char* TAG = "node_cli";

/* ── Configuration ─────────────────────────────────────────── */

#define NODE_CLIENT_STACK (16 * 1024)
#define NODE_CLIENT_PRIO 45
#define NODE_READ_BUF_SIZE 8192
#define NODE_TOOL_OUTPUT_SIZE (8 * 1024)

/* ── TLS + Socket state ────────────────────────────────────── */

typedef struct {
    int fd;
    bool use_tls;
    bool connected;

    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool tls_init;
} node_ws_t;

static node_ws_t s_ws;
static volatile bool s_running = false;
static pthread_t s_thread;
static bool s_thread_valid = false;
static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static char s_gateway_host[128];
static int s_gateway_port = 0;
static char s_gateway_token[256];
static bool s_use_tls = false;

/* ── Entropy ───────────────────────────────────────────────── */

static int entropy_func(void* data, unsigned char* output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0) {
        return 0;
    }
    syslog(LOG_ERR, "[node_client] CRITICAL: No secure entropy source available\n");
    return -1; /* Generic error - TLS handshake will fail safely */
}

/* ── Raw I/O ───────────────────────────────────────────────── */

static int raw_read(node_ws_t* ws, void* buf, size_t len)
{
    if (ws->use_tls) {
        int n;
        do {
            n = mbedtls_ssl_read(&ws->ssl, buf, len);
        } while (n == MBEDTLS_ERR_SSL_WANT_READ);
        return n;
    }
    return (int)recv(ws->fd, buf, len, 0);
}

static int raw_write(node_ws_t* ws, const void* buf, size_t len)
{
    if (ws->use_tls) {
        size_t written = 0;
        while (written < len) {
            int n = mbedtls_ssl_write(&ws->ssl, (const unsigned char*)buf + written,
                len - written);
            if (n == MBEDTLS_ERR_SSL_WANT_WRITE)
                continue;
            if (n <= 0)
                return -1;
            written += (size_t)n;
        }
        return (int)written;
    }
    return (int)send(ws->fd, buf, len, 0);
}

static int read_exact(node_ws_t* ws, void* buf, size_t len)
{
    size_t total = 0;
    while (total < len) {
        int n = raw_read(ws, (char*)buf + total, len - total);
        if (n <= 0)
            return -1;
        total += (size_t)n;
    }
    return (int)total;
}

/* ── WebSocket frame send (masked, as client) ──────────────── */

static int ws_send_text(node_ws_t* ws, const char* data, size_t len)
{
    pthread_mutex_lock(&s_mutex);
    if (!ws->connected) {
        pthread_mutex_unlock(&s_mutex);
        return -1;
    }

    int result = -1;
    uint8_t header[14];
    int hlen = 0;
    header[hlen++] = 0x81; /* FIN + text */

    if (len < 126) {
        header[hlen++] = (uint8_t)(0x80 | len);
    } else if (len < 65536) {
        header[hlen++] = 0x80 | 126;
        header[hlen++] = (uint8_t)((len >> 8) & 0xFF);
        header[hlen++] = (uint8_t)(len & 0xFF);
    } else {
        header[hlen++] = 0x80 | 127;
        for (int i = 7; i >= 0; i--)
            header[hlen++] = (uint8_t)((len >> (i * 8)) & 0xFF);
    }

    uint8_t mask[4];
    if (entropy_func(NULL, mask, 4) != 0)
        goto out;
    header[hlen++] = mask[0];
    header[hlen++] = mask[1];
    header[hlen++] = mask[2];
    header[hlen++] = mask[3];

    if (raw_write(ws, header, hlen) < 0)
        goto out;

    const uint8_t* src = (const uint8_t*)data;
    uint8_t chunk[256];
    for (size_t off = 0; off < len;) {
        size_t todo = len - off;
        if (todo > sizeof(chunk))
            todo = sizeof(chunk);
        for (size_t i = 0; i < todo; i++)
            chunk[i] = src[off + i] ^ mask[(off + i) & 3];
        if (raw_write(ws, chunk, todo) < 0)
            goto out;
        off += todo;
    }
    result = 0;

out:
    pthread_mutex_unlock(&s_mutex);
    return result;
}

/* ── Protocol helpers ──────────────────────────────────────── */

static void gen_id(char* buf, size_t cap)
{
    unsigned char rnd[6];
    if (agent_secure_random(rnd, sizeof(rnd)) == 0) {
        snprintf(buf, cap, "%02x%02x%02x%02x-%02x%02x",
            rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5]);
    } else {
        /* Fallback: use address + time as unique-ish ID (not for crypto) */
        snprintf(buf, cap, "%08lx-%04x", (unsigned long)time(NULL),
            (unsigned)((uintptr_t)buf & 0xFFFF));
    }
}

static int send_json_request(const char* method, cJSON* params)
{
    char id[32];
    gen_id(id, sizeof(id));

    cJSON* frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "req");
    cJSON_AddStringToObject(frame, "id", id);
    cJSON_AddStringToObject(frame, "method", method);
    cJSON_AddItemToObject(frame, "params",
        params ? params : cJSON_CreateObject());

    char* json = cJSON_PrintUnformatted(frame);
    if (!json) {
        syslog(LOG_ERR, "[%s] JSON serialization failed (OOM)\n", TAG);
        cJSON_Delete(frame);
        return -1;
    }
    int ret = ws_send_text(&s_ws, json, strlen(json));
    syslog(LOG_DEBUG, "[%s] TX: %s\n", TAG, method);
    free(json);
    cJSON_Delete(frame);
    return ret;
}

/* ── Build command list from tool_registry ──────────────────── */

static cJSON* build_commands_array(void)
{
    cJSON* cmds = cJSON_CreateArray();
    const char* tools_json = tool_registry_get_tools_json();
    if (!tools_json)
        return cmds;

    cJSON* tools = cJSON_Parse(tools_json);
    if (!tools)
        return cmds;

    cJSON* tool = NULL;
    cJSON_ArrayForEach(tool, tools)
    {
        cJSON* name = cJSON_GetObjectItem(tool, "name");
        if (name && cJSON_IsString(name))
            cJSON_AddItemToArray(cmds, cJSON_CreateString(name->valuestring));
    }
    cJSON_Delete(tools);
    return cmds;
}

/* ── Send connect request ──────────────────────────────────── */

static void send_connect(void)
{
    cJSON* params = cJSON_CreateObject();
    cJSON_AddNumberToObject(params, "minProtocol", 3);
    cJSON_AddNumberToObject(params, "maxProtocol", 3);

    cJSON* client = cJSON_CreateObject();
    cJSON_AddStringToObject(client, "id", AGENT_NODE_ID);
    cJSON_AddStringToObject(client, "displayName", AGENT_NODE_DISPLAY_NAME);
    cJSON_AddStringToObject(client, "version", "1.0.0");
    cJSON_AddStringToObject(client, "platform", "vela");
    cJSON_AddStringToObject(client, "deviceFamily", "watch");
    cJSON_AddStringToObject(client, "mode", "node");
    cJSON_AddItemToObject(params, "client", client);

    cJSON* caps = cJSON_CreateArray();
    cJSON_AddItemToArray(caps, cJSON_CreateString("node.invoke"));
    cJSON_AddItemToObject(params, "caps", caps);

    cJSON_AddItemToObject(params, "commands", build_commands_array());
    cJSON_AddStringToObject(params, "role", "node");
    cJSON_AddItemToObject(params, "scopes", cJSON_CreateArray());

    if (s_gateway_token[0]) {
        cJSON* auth = cJSON_CreateObject();
        cJSON_AddStringToObject(auth, "token", s_gateway_token);
        cJSON_AddItemToObject(params, "auth", auth);
    }

    send_json_request("connect", params);
}

/* ── Handle node.invoke.request ────────────────────────────── */

static void handle_invoke(cJSON* payload)
{
    const char* invoke_id = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "id"));
    const char* node_id = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "nodeId"));
    const char* command = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "command"));

    cJSON* params_item = cJSON_GetObjectItem(payload, "paramsJSON");
    const char* params_json = cJSON_GetStringValue(params_item);
    if (!params_json) {
        cJSON* fallback = params_item;
        if (!fallback || !cJSON_IsObject(fallback))
            fallback = cJSON_GetObjectItem(payload, "params");
        if (fallback && cJSON_IsObject(fallback)) {
            params_json = cJSON_GetStringValue(fallback);
        }
    }

    if (!invoke_id || !command)
        return;

    syslog(LOG_DEBUG, "[%s] invoke: %s (id=%.8s)\n", TAG, command, invoke_id);

    /* Execute via tool_registry */
    char* output = malloc(NODE_TOOL_OUTPUT_SIZE);
    if (!output)
        return;

    output[0] = '\0';
    int ret = tool_registry_execute(command, params_json ? params_json : "{}",
        output, NODE_TOOL_OUTPUT_SIZE);

    syslog(LOG_DEBUG, "[%s] tool_registry_execute(%s) ret=%d\n", TAG,
        command, ret);

    /* Build result */
    cJSON* res_params = cJSON_CreateObject();
    cJSON_AddStringToObject(res_params, "id", invoke_id);
    if (node_id)
        cJSON_AddStringToObject(res_params, "nodeId", node_id);
    cJSON_AddBoolToObject(res_params, "ok", ret == OK);

    if (ret == OK) {
        cJSON_AddStringToObject(res_params, "payloadJSON", output);
    } else {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "code", "COMMAND_ERROR");
        cJSON_AddStringToObject(err, "message",
            output[0] ? output : "tool execution failed");
        cJSON_AddItemToObject(res_params, "error", err);
    }

    send_json_request("node.invoke.result", res_params);
    free(output);
}

/* ── Forward declarations ──────────────────────────────────── */
static void do_disconnect_locked(void); /* caller must hold s_mutex */

/* ── TCP + TLS + WS handshake ──────────────────────────────── */
static int do_connect(void)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", s_gateway_port);

    memset(&s_ws, 0, sizeof(s_ws));
    s_ws.fd = -1;
    s_ws.use_tls = s_use_tls;

    if (s_use_tls) {
        mbedtls_net_init(&s_ws.net);
        mbedtls_ssl_init(&s_ws.ssl);
        mbedtls_ssl_config_init(&s_ws.conf);
        mbedtls_ctr_drbg_init(&s_ws.ctr_drbg);

        if (mbedtls_ctr_drbg_seed(&s_ws.ctr_drbg, entropy_func, NULL, NULL, 0) != 0)
            goto fail;

        int ret = mbedtls_net_connect(&s_ws.net, s_gateway_host, port_str,
            MBEDTLS_NET_PROTO_TCP);
        if (ret != 0) {
            syslog(LOG_ERR, "[%s] net_connect %s:%s: -0x%04x\n", TAG, s_gateway_host,
                port_str, -ret);
            goto fail;
        }

        mbedtls_net_set_block(&s_ws.net);
        struct timeval tv = { .tv_sec = 30 };
        setsockopt(s_ws.net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        mbedtls_ssl_config_defaults(&s_ws.conf, MBEDTLS_SSL_IS_CLIENT,
            MBEDTLS_SSL_TRANSPORT_STREAM,
            MBEDTLS_SSL_PRESET_DEFAULT);
        /* VERIFY_OPTIONAL: no CA bundle on embedded, but still log cert warnings.
         * Consistent with feishu_bot.c / vela_tls.c behaviour. */
        mbedtls_ssl_conf_authmode(&s_ws.conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
        mbedtls_ssl_conf_rng(&s_ws.conf, mbedtls_ctr_drbg_random, &s_ws.ctr_drbg);

        if (mbedtls_ssl_setup(&s_ws.ssl, &s_ws.conf) != 0)
            goto fail;
        mbedtls_ssl_set_hostname(&s_ws.ssl, s_gateway_host);
        mbedtls_ssl_set_bio(&s_ws.ssl, &s_ws.net, mbedtls_net_send,
            mbedtls_net_recv, NULL);

        while ((ret = mbedtls_ssl_handshake(&s_ws.ssl)) != 0) {
            if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                syslog(LOG_ERR, "[%s] TLS handshake: -0x%04x\n", TAG, -ret);
                goto fail;
            }
        }
        s_ws.fd = s_ws.net.fd;
        s_ws.tls_init = true;
    } else {
        struct addrinfo hints = { 0 }, *res = NULL;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(s_gateway_host, port_str, &hints, &res) != 0 || !res)
            return -1;

        s_ws.fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (s_ws.fd < 0) {
            freeaddrinfo(res);
            return -1;
        }
        if (connect(s_ws.fd, res->ai_addr, res->ai_addrlen) < 0) {
            close(s_ws.fd);
            s_ws.fd = -1;
            freeaddrinfo(res);
            return -1;
        }
        struct timeval tv = { .tv_sec = 30 };
        setsockopt(s_ws.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        freeaddrinfo(res);
    }

    /* WebSocket upgrade handshake */
    uint8_t raw_key[16];
    if (entropy_func(NULL, raw_key, sizeof(raw_key)) != 0) {
        syslog(LOG_ERR, "[%s] Failed to generate WS key entropy\n", TAG);
        goto fail;
    }
    unsigned char b64_key[32] = { 0 };
    size_t b64_len = 0;
    mbedtls_base64_encode(b64_key, sizeof(b64_key) - 1, &b64_len, raw_key,
        sizeof(raw_key));

    char req[512];
    int rlen = snprintf(req, sizeof(req),
        "GET / HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n",
        s_gateway_host, s_gateway_port, (char*)b64_key);

    if (raw_write(&s_ws, req, rlen) < 0)
        goto fail;

    char resp[1024] = { 0 };
    int resp_len = 0;
    while (resp_len < (int)sizeof(resp) - 1) {
        int n = raw_read(&s_ws, resp + resp_len, 1);
        if (n <= 0)
            goto fail;
        resp_len += n;
        if (resp_len >= 4 && memcmp(resp + resp_len - 4, "\r\n\r\n", 4) == 0)
            break;
    }

    if (!strstr(resp, " 101 ")) {
        syslog(LOG_ERR, "[%s] WS upgrade failed\n", TAG);
        goto fail;
    }

    pthread_mutex_lock(&s_mutex);
    s_ws.connected = true;
    pthread_mutex_unlock(&s_mutex);
    syslog(LOG_INFO, "[%s] WebSocket connected to %s:%d\n", TAG, s_gateway_host,
        s_gateway_port);
    return 0;

fail:
    pthread_mutex_lock(&s_mutex);
    do_disconnect_locked();
    pthread_mutex_unlock(&s_mutex);
    return -1;
}

/* Phase 1: Close the underlying fd so any blocking recv/read returns
 * immediately with an error.  Does NOT free TLS objects — the worker
 * thread may still be unwinding through mbedtls_ssl_read when this
 * runs.  Caller must hold s_mutex. */
static void do_shutdown_fd_locked(void)
{
    s_ws.connected = false;
    if (s_ws.tls_init) {
        /* Shut down the transport fd; mbedtls_ssl_read will return error */
        if (s_ws.net.fd >= 0) {
            shutdown(s_ws.net.fd, SHUT_RDWR);
            close(s_ws.net.fd);
            s_ws.net.fd = -1;
        }
    } else if (s_ws.fd >= 0) {
        shutdown(s_ws.fd, SHUT_RDWR);
        close(s_ws.fd);
    }
    s_ws.fd = -1;
}

/* Phase 2: Free TLS library objects.  Only safe to call after the
 * worker thread has exited (i.e. after pthread_join) or from the
 * worker thread itself.  Caller must hold s_mutex. */
static void do_free_tls_locked(void)
{
    if (s_ws.tls_init) {
        mbedtls_ssl_free(&s_ws.ssl);
        mbedtls_ssl_config_free(&s_ws.conf);
        mbedtls_ctr_drbg_free(&s_ws.ctr_drbg);
        /* net fd already closed in phase 1; just reset the context */
        mbedtls_net_init(&s_ws.net);
        s_ws.tls_init = false;
    }
}

/* Full disconnect (both phases).  Used by the worker thread itself
 * where there is no concurrent reader to race against. */
static void do_disconnect_locked(void)
{
    do_shutdown_fd_locked();
    do_free_tls_locked();
}

/* Thread-safe full disconnect for the worker thread's own use */
static void do_disconnect(void)
{
    pthread_mutex_lock(&s_mutex);
    do_disconnect_locked();
    pthread_mutex_unlock(&s_mutex);
}

/* Interrupt-only disconnect for external callers (stop/start).
 * Closes the fd to unblock the worker, but does NOT free TLS objects.
 * Caller must pthread_join the worker, then call do_free_tls. */
static void do_shutdown_fd(void)
{
    pthread_mutex_lock(&s_mutex);
    do_shutdown_fd_locked();
    pthread_mutex_unlock(&s_mutex);
}

static void do_free_tls(void)
{
    pthread_mutex_lock(&s_mutex);
    do_free_tls_locked();
    pthread_mutex_unlock(&s_mutex);
}

/* ── WS recv + message dispatch ────────────────────────────── */

static int ws_recv_frame(node_ws_t* ws, char* buf, size_t buf_size)
{
    uint8_t b0, b1;
    if (read_exact(ws, &b0, 1) < 0)
        return -1;
    if (read_exact(ws, &b1, 1) < 0)
        return -1;

    uint8_t opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    uint64_t plen = b1 & 0x7F;

    if (opcode == 0x08)
        return 0; /* close */
    if (plen == 126) {
        uint8_t ext[2];
        if (read_exact(ws, ext, 2) < 0)
            return -1;
        plen = ((uint64_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        uint8_t ext[8];
        if (read_exact(ws, ext, 8) < 0)
            return -1;
        plen = 0;
        for (int i = 0; i < 8; i++)
            plen = (plen << 8) | ext[i];
    }

    uint8_t mask_key[4] = { 0 };
    if (masked && read_exact(ws, mask_key, 4) < 0)
        return -1;

    size_t read_len = plen < (buf_size - 1) ? (size_t)plen : (buf_size - 1);
    if (read_exact(ws, buf, read_len) < 0)
        return -1;

    /* discard overflow */
    for (uint64_t i = read_len; i < plen; i++) {
        uint8_t d;
        if (read_exact(ws, &d, 1) < 0)
            return -1;
    }

    if (masked) {
        for (size_t i = 0; i < read_len; i++)
            buf[i] ^= mask_key[i & 3];
    }
    buf[read_len] = '\0';

    if (opcode == 0x09) { /* ping → pong */
        uint8_t pong_hdr[6];
        pong_hdr[0] = 0x8A; /* FIN + pong */
        pong_hdr[1] = 0x80 | (uint8_t)read_len; /* masked, same payload */
        uint8_t pmask[4];
        if (entropy_func(NULL, pmask, 4) != 0)
            return -1;
        memcpy(pong_hdr + 2, pmask, 4);
        raw_write(ws, pong_hdr, 6);
        if (read_len > 0) {
            uint8_t pong_body[125]; /* ping payload max 125 bytes per RFC */
            size_t pong_len = read_len < sizeof(pong_body) ? read_len : sizeof(pong_body);
            for (size_t i = 0; i < pong_len; i++)
                pong_body[i] = (uint8_t)buf[i] ^ pmask[i & 3];
            raw_write(ws, pong_body, pong_len);
        }
        return -2;
    }
    return (opcode == 0x01 || opcode == 0x02) ? (int)read_len : -2;
}

static void on_message(const char* data, int len)
{
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (!root) {
        syslog(LOG_ERR, "[%s] JSON parse failed\n", TAG);
        return;
    }

    const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (!type) {
        syslog(LOG_WARNING, "[%s] no 'type' field\n", TAG);
        cJSON_Delete(root);
        return;
    }

    syslog(LOG_DEBUG, "[%s] msg type=%s\n", TAG, type);

    if (strcmp(type, "evt") == 0 || strcmp(type, "event") == 0) {
        const char* event = cJSON_GetStringValue(cJSON_GetObjectItem(root, "event"));
        if (!event) {
            syslog(LOG_WARNING, "[%s] evt without 'event' field\n", TAG);
            cJSON_Delete(root);
            return;
        }

        if (strcmp(event, "connect.challenge") == 0) {
            syslog(LOG_INFO, "[%s] Got challenge, sending connect\n", TAG);
            send_connect();
        } else if (strcmp(event, "chat.forward") == 0) {
            /* Gateway forwarded a chat message (e.g. bot-to-bot @mention).
             * Inject it into our local inbound message bus so the agent
             * processes it as if it arrived via the normal channel. */
            cJSON* pl = cJSON_GetObjectItem(root, "payload");
            if (pl) {
                const char* ch = cJSON_GetStringValue(
                    cJSON_GetObjectItem(pl, "channel"));
                const char* cid = cJSON_GetStringValue(
                    cJSON_GetObjectItem(pl, "chat_id"));
                const char* ct = cJSON_GetStringValue(
                    cJSON_GetObjectItem(pl, "content"));
                if (ch && cid && ct && ct[0]) {
                    agent_msg_t m = { 0 };
                    strncpy(m.channel, ch, sizeof(m.channel) - 1);
                    strncpy(m.chat_id, cid, sizeof(m.chat_id) - 1);
                    m.content = strdup(ct);
                    if (m.content) {
                        if (message_bus_push_inbound(&m) == OK) {
                            syslog(LOG_INFO,
                                "[%s] chat.forward injected: %s:%.24s\n",
                                TAG, ch, cid);
                        } else {
                            free(m.content);
                        }
                    }
                }
            }
        } else if (strcmp(event, "node.invoke.request") == 0) {
            cJSON* pl = cJSON_GetObjectItem(root, "payload");
            if (pl)
                handle_invoke(pl);
            else
                syslog(LOG_WARNING, "[%s] invoke event but no payload\n", TAG);
        }
    } else if (strcmp(type, "res") == 0) {
        bool ok = cJSON_IsTrue(cJSON_GetObjectItem(root, "ok"));
        if (ok) {
            cJSON* pl = cJSON_GetObjectItem(root, "payload");
            const char* res_type = cJSON_GetStringValue(cJSON_GetObjectItem(pl, "type"));
            if (res_type && strcmp(res_type, "hello-ok") == 0) {
                syslog(LOG_INFO, "[%s] Connected to Gateway as Node\n", TAG);
            }
        } else {
            cJSON* err = cJSON_GetObjectItem(root, "error");
            const char* msg = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
            syslog(LOG_ERR, "[%s] Gateway error: %s\n", TAG, msg ? msg : "unknown");
        }
    } else if (strcmp(type, "req") == 0) {
        const char* method = cJSON_GetStringValue(cJSON_GetObjectItem(root, "method"));
        if (method && strcmp(method, "node.invoke") == 0) {
            cJSON* params = cJSON_GetObjectItem(root, "params");
            if (params)
                handle_invoke(params);
            else
                syslog(LOG_WARNING, "[%s] node.invoke req but no params\n", TAG);
        }
    }

    cJSON_Delete(root);
}

/* ── Main thread ───────────────────────────────────────────── */

static void* node_client_thread(void* arg)
{
    (void)arg;
    char* buf = malloc(NODE_READ_BUF_SIZE);
    if (!buf)
        return NULL;

    while (s_running) {
        /* Skip connection if gateway not configured */
        if (s_gateway_host[0] == '\0' || s_gateway_port == 0) {
            sleep(30);
            continue;
        }

        syslog(LOG_INFO, "[%s] Connecting to %s:%d (tls=%d)...\n", TAG,
            s_gateway_host, s_gateway_port, s_use_tls);

        if (do_connect() != 0) {
            syslog(LOG_WARNING, "[%s] Connect failed, retry in 10s\n", TAG);
            sleep(10);
            continue;
        }

        /* Recv loop — send ping every 30s to keep connection alive */
        time_t last_ping = time(NULL);
        while (s_running && s_ws.connected) {
            int n = ws_recv_frame(&s_ws, buf, NODE_READ_BUF_SIZE);
            if (n == 0)
                break; /* close frame */
            if (n == -1) {
                /* Check if this is a recv timeout (not a real error) */
                time_t now = time(NULL);
                if (now - last_ping >= 25) {
                    /* Send a WebSocket ping to keep alive */
                    uint8_t ping[6];
                    ping[0] = 0x89; /* FIN + ping */
                    ping[1] = 0x80; /* masked, 0 payload */
                    if (entropy_func(NULL, ping + 2, 4) != 0) /* mask key */
                        break;
                    if (raw_write(&s_ws, ping, 6) < 0)
                        break;
                    last_ping = now;
                    continue;
                }
                break; /* real error or second timeout without pong */
            }
            if (n == -2)
                continue; /* ignored frame (ping/pong) */
            on_message(buf, n);
            last_ping = time(NULL); /* any data resets the ping timer */
        }

        do_disconnect();
        if (!s_running)
            break;
        syslog(LOG_INFO, "[%s] Disconnected, reconnecting in 5s\n", TAG);
        sleep(5);
    }

    free(buf);
    syslog(LOG_INFO, "[%s] Node client thread exited\n", TAG);
    return NULL;
}

/* ── Public API ────────────────────────────────────────────── */

int node_client_init(void)
{
    syslog(LOG_INFO, "[%s] Node client initialized\n", TAG);
    return OK;
}

int node_client_start(void)
{
    /* Stop previous instance if running */
    if (s_running) {
        syslog(LOG_INFO, "[%s] Stopping previous client...\n", TAG);
        s_running = false;
        do_shutdown_fd();
        /* Wait for old thread to actually exit */
        if (s_thread_valid) {
            pthread_join(s_thread, NULL);
            s_thread_valid = false;
        }
        do_free_tls();
    }

    /* Read gateway config */
    char host_buf[128] = { 0 };
    char port_buf[16] = { 0 };
    char token_buf[256] = { 0 };
    claw_config_get(AGENT_CFG_KEY_GATEWAY_HOST, host_buf, sizeof(host_buf));
    claw_config_get(AGENT_CFG_KEY_GATEWAY_PORT, port_buf, sizeof(port_buf));
    claw_config_get(AGENT_CFG_KEY_GATEWAY_TOKEN, token_buf, sizeof(token_buf));

    if (!host_buf[0]) {
        syslog(LOG_INFO, "[%s] No gateway_host configured, node client disabled\n",
            TAG);
        return OK; /* not an error — just not configured */
    }

    memset(s_gateway_host, 0, sizeof(s_gateway_host));
    strncpy(s_gateway_host, host_buf, sizeof(s_gateway_host) - 1);
    s_gateway_port = port_buf[0] ? atoi(port_buf) : 8080;
    memset(s_gateway_token, 0, sizeof(s_gateway_token));
    if (token_buf[0])
        strncpy(s_gateway_token, token_buf, sizeof(s_gateway_token) - 1);
    s_use_tls = (s_gateway_port == 443);

    s_running = true;

    /* Create joinable thread (not detached) so we can synchronize on stop */
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, NODE_CLIENT_STACK < 4096 ? 4096 : NODE_CLIENT_STACK);

    struct sched_param sp;
    sp.sched_priority = NODE_CLIENT_PRIO;
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

    int ret = pthread_create(&s_thread, &attr, node_client_thread, NULL);
    pthread_attr_destroy(&attr);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] Failed to create node client thread: %d\n", TAG, ret);
        s_running = false;
        return ERROR;
    }
    s_thread_valid = true;

    syslog(LOG_INFO, "[%s] Node client started → %s:%d\n", TAG, s_gateway_host,
        s_gateway_port);
    return OK;
}

void node_client_stop(void)
{
    s_running = false;
    /* Phase 1: close fd to unblock any blocking recv in the worker */
    do_shutdown_fd();
    /* Wait for worker thread to fully exit */
    if (s_thread_valid) {
        pthread_join(s_thread, NULL);
        s_thread_valid = false;
    }
    /* Phase 2: now safe to free TLS objects — worker is gone */
    do_free_tls();
    syslog(LOG_INFO, "[%s] Node client stopped\n", TAG);
}

int node_client_send_chat_message(const char* channel, const char* chat_id,
    const char* content)
{
    pthread_mutex_lock(&s_mutex);
    if (!s_ws.connected) {
        pthread_mutex_unlock(&s_mutex);
        syslog(LOG_WARNING, "[%s] send_chat_message: not connected\n", TAG);
        return ERROR;
    }
    pthread_mutex_unlock(&s_mutex);

    /* Use chat.forward — the same event name the gateway uses to push
     * messages to nodes, so the gateway already understands this payload
     * shape and can route it to the appropriate channel (e.g. feishu). */
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "channel", channel);
    cJSON_AddStringToObject(payload, "chat_id", chat_id);
    cJSON_AddStringToObject(payload, "content", content);

    cJSON* params = cJSON_CreateObject();
    cJSON_AddItemToObject(params, "payload", payload);

    int ret = send_json_request("chat.forward", params);
    if (ret != 0) {
        syslog(LOG_WARNING, "[%s] send_chat_message failed\n", TAG);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Forwarded chat message → gateway %s:%s\n",
        TAG, channel, chat_id);
    return OK;
}
