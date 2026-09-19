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

#include "channels/ws_server.h"
#include "core/message_bus.h"
#ifdef CONFIG_AI_AGENT_NODE
#include "node/node_manager.h"
#endif
#include "agent_compat.h"
#include "agent_config.h"
#ifdef CONFIG_AI_AGENT_REST_API
#include "infra/api_handler.h"
#endif
#ifdef CONFIG_AI_AGENT_LVGL_UI
#include "ui/lvgl_ui_channel.h"
#endif

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* POSIX sockets */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>

/* mbedTLS SHA-1 + Base64 */
#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"

static const char* TAG = "ws";

/* ── WS Magic GUID ────────────────────────────────────────────── */

#define WS_GUID "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"

/* ── Client table ─────────────────────────────────────────────── */

typedef struct {
    int fd;
    char chat_id[32];
    bool active;
} ws_client_t;

static ws_client_t s_clients[AGENT_WS_MAX_CLIENTS];
static pthread_mutex_t s_clients_mtx = PTHREAD_MUTEX_INITIALIZER;

static int s_listen_fd = -1;
static volatile bool s_running = false;

/* ── Undelivered replies ──────────────────────────────────────── */

/* The phone page keeps a WebSocket open, but our bt-pan link drops for tens
 * of seconds at a time, and a cloud call that meets a drop can take a minute
 * to fail before the on-device model answers.  The answer used to be dropped
 * here (only a log line), which on the phone looked exactly like "发了消息，
 * 却没有回复".  Hold the last few undelivered replies per chat_id and send
 * them as soon as that chat_id is back. */
#define WS_PENDING_MAX   3     /* replies held per chat_id */
#define WS_PENDING_LEN   768   /* bytes per held reply */
#define WS_PENDING_SLOTS 2     /* chat_ids tracked (the phone page uses one) */

typedef struct {
    char chat_id[32];
    int head;                    /* next slot to overwrite */
    int count;
    char text[WS_PENDING_MAX][WS_PENDING_LEN];
    bool from_local[WS_PENDING_MAX];   /* keep the 本地/云端 label with the text */
} ws_pending_t;

static ws_pending_t s_pending[WS_PENDING_SLOTS];

static void pending_flush(const char* chat_id, int fd);

/* Call with s_clients_mtx held. */
static ws_pending_t* pending_slot_locked(const char* chat_id, bool create)
{
    ws_pending_t* free_slot = NULL;
    int i;

    for (i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
        if (s_pending[i].chat_id[0] != '\0' &&
            strcmp(s_pending[i].chat_id, chat_id) == 0) {
            return &s_pending[i];
        }
        if (s_pending[i].chat_id[0] == '\0' && free_slot == NULL) {
            free_slot = &s_pending[i];
        }
    }
    if (!create || free_slot == NULL) {
        return NULL;
    }
    snprintf(free_slot->chat_id, sizeof(free_slot->chat_id), "%s", chat_id);
    free_slot->head = 0;
    free_slot->count = 0;
    return free_slot;
}

/* Call with s_clients_mtx held. */
static void pending_hold_locked(const char* chat_id, const char* text,
    bool from_local)
{
    ws_pending_t* p = pending_slot_locked(chat_id, true);

    if (p == NULL) {
        return;
    }
    snprintf(p->text[p->head], WS_PENDING_LEN, "%s", text);
    p->from_local[p->head] = from_local;
    p->head = (p->head + 1) % WS_PENDING_MAX;
    if (p->count < WS_PENDING_MAX) {
        p->count++;
    }
    syslog(LOG_INFO, "[%s] holding reply for %s (%d pending)\n", TAG, chat_id,
        p->count);
}

/* ── Client helpers (call with mtx held) ──────────────────────── */

static ws_client_t* find_by_fd_locked(int fd)
{
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd)
            return &s_clients[i];
    }
    return NULL;
}

static ws_client_t* find_by_chat_id_locked(const char* chat_id)
{
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && strcmp(s_clients[i].chat_id, chat_id) == 0)
            return &s_clients[i];
    }
    return NULL;
}

static ws_client_t* add_client_locked(int fd)
{
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
        if (!s_clients[i].active) {
            s_clients[i].fd = fd;
            s_clients[i].active = true;
            snprintf(s_clients[i].chat_id, sizeof(s_clients[i].chat_id), "ws_%d", fd);
            syslog(LOG_INFO, "[%s] Client connected: %s (fd=%d)\n", TAG,
                s_clients[i].chat_id, fd);
            return &s_clients[i];
        }
    }
    return NULL;
}

static void remove_client_locked(int fd)
{
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd) {
            syslog(LOG_INFO, "[%s] Client disconnected: %s\n", TAG,
                s_clients[i].chat_id);
            s_clients[i].active = false;
            s_clients[i].fd = -1; /* invalidate fd before closing */
            close(fd);
            return;
        }
    }
}

/* ── WS handshake ─────────────────────────────────────────────── */

/**
 * Compute Sec-WebSocket-Accept = base64(SHA1(key + GUID))
 */
static int make_accept_key(const char* key, char* out, size_t out_size)
{
    char combined[256];
    int clen = snprintf(combined, sizeof(combined), "%s%s", key, WS_GUID);
    if (clen <= 0 || clen >= (int)sizeof(combined))
        return -1;

    unsigned char sha[20];
    if (mbedtls_sha1((const unsigned char*)combined, (size_t)clen, sha) != 0)
        return -1;

    size_t written = 0;
    if (mbedtls_base64_encode((unsigned char*)out, out_size, &written, sha,
            sizeof(sha))
        != 0)
        return -1;
    out[written] = '\0';
    return (int)written;
}

/**
 * WebSocket handshake using pre-read buffer.
 * Read full HTTP upgrade request, extract Sec-WebSocket-Key.
 * Returns 0 on success; sends 101 response.
 */
/* Write exactly len bytes: lwIP's send() may accept only part of the buffer
 * even on a blocking socket, and a short 101 response leaves the browser
 * waiting for the rest of the handshake. */
static int send_all(int fd, const char* buf, size_t len)
{
    size_t left = len;

    while (left > 0) {
        ssize_t n = send(fd, buf, left, 0);
        if (n <= 0) {
            return -1;
        }
        buf += n;
        left -= (size_t)n;
    }
    return 0;
}

static int do_ws_handshake_ex(int fd, const char* buf, int buf_len,
    char* chat_id_out, size_t chat_id_size)
{

    /* Extract Sec-WebSocket-Key */
    char* key_hdr = strcasestr(buf, "\r\nSec-WebSocket-Key: ");
    if (!key_hdr) {
        syslog(LOG_WARNING, "[%s] No Sec-WebSocket-Key (fd=%d)\n", TAG, fd);
        return -1;
    }
    key_hdr += 21;
    char* eol = strstr(key_hdr, "\r\n");
    if (!eol) {
        syslog(LOG_WARNING, "[%s] key not terminated (fd=%d)\n", TAG, fd);
        return -1;
    }

    char ws_key[128] = { 0 };
    size_t klen = (size_t)(eol - key_hdr);
    if (klen >= sizeof(ws_key)) {
        syslog(LOG_WARNING, "[%s] key too long (%d, fd=%d)\n", TAG,
            (int)klen, fd);
        return -1;
    }
    memcpy(ws_key, key_hdr, klen);
    ws_key[klen] = '\0';

    /* Compute accept */
    char accept[64] = { 0 };
    if (make_accept_key(ws_key, accept, sizeof(accept)) < 0) {
        syslog(LOG_WARNING, "[%s] accept key failed (fd=%d)\n", TAG, fd);
        return -1;
    }

    /* Send 101 */
    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        accept);
    if (send_all(fd, resp, (size_t)rlen) != 0) {
        syslog(LOG_WARNING, "[%s] 101 send failed (fd=%d)\n", TAG, fd);
        return -1;
    }

    /* Default chat_id from fd (may be overridden by first message) */
    snprintf(chat_id_out, chat_id_size, "ws_%d", fd);
    return 0;
}

/* ── WS frame encode/decode ───────────────────────────────────── */

/**
 * Send a text frame (server → client, no masking).
 * Caller must hold s_clients_mtx.
 */
static int ws_send_frame(int fd, const char* payload, size_t len)
{
    unsigned char hdr[10];
    int hdr_len = 0;

    hdr[0] = 0x81; /* FIN + text opcode */
    if (len < 126) {
        hdr[1] = (unsigned char)len;
        hdr_len = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (unsigned char)(len >> 8);
        hdr[3] = (unsigned char)(len & 0xFF);
        hdr_len = 4;
    } else {
        syslog(LOG_ERR, "[%s] Payload too large: %d\n", TAG, (int)len);
        return -1;
    }

    if (send(fd, hdr, hdr_len, 0) != hdr_len)
        return -1;
    if (send(fd, payload, len, 0) != (int)len)
        return -1;
    return 0;
}

/**
 * Read and decode one WS frame; place text payload into buf (NUL-terminated).
 * Returns payload length on success, 0 on close frame, -1 on error.
 */
static int ws_recv_frame(int fd, char* buf, size_t buf_size)
{
    unsigned char b0, b1;
    if (recv(fd, &b0, 1, MSG_WAITALL) != 1)
        return -1;
    if (recv(fd, &b1, 1, MSG_WAITALL) != 1)
        return -1;

    int opcode = b0 & 0x0F;
    bool masked = (b1 & 0x80) != 0;
    size_t plen = b1 & 0x7F;

    if (opcode == 0x8)
        return 0; /* Close frame */

    if (opcode == 0x9 || opcode == 0xA) {
        /* Ping (0x9) or Pong (0xA) — must consume payload before returning,
         * otherwise leftover bytes corrupt the next frame read. */
        if (plen == 126) {
            unsigned char ext[2];
            if (recv(fd, ext, 2, MSG_WAITALL) != 2)
                return -1;
            plen = ((size_t)ext[0] << 8) | ext[1];
        } else if (plen == 127) {
            return -1;
        }

        unsigned char pp_mask[4] = { 0 };
        if (masked) {
            if (recv(fd, pp_mask, 4, MSG_WAITALL) != 4)
                return -1;
        }

        if (plen > 125) {
            syslog(LOG_ERR,
                "[%s] Invalid control frame payload length: %d\n",
                TAG, (int)plen);
            return -1;
        }

        /* Read full control payload so ping can be echoed in pong. */
        unsigned char ctrl_payload[125] = { 0 };
        if (plen > 0) {
            if (recv(fd, ctrl_payload, plen, MSG_WAITALL) != (int)plen)
                return -1;

            if (masked) {
                for (size_t i = 0; i < plen; i++) {
                    ctrl_payload[i] ^= pp_mask[i % 4];
                }
            }
        }

        /* Reply with pong if it was a ping */
        if (opcode == 0x9) {
            unsigned char pong_hdr[2] = { 0x8A, (unsigned char)plen };
            send(fd, pong_hdr, 2, 0);
            if (plen > 0) {
                send(fd, ctrl_payload, plen, 0);
            }
        }
        return -2;
    }

    if (opcode != 0x1 && opcode != 0x2) {
        /* Unknown opcode — skip */
        return -2;
    }

    if (plen == 126) {
        unsigned char ext[2];
        if (recv(fd, ext, 2, MSG_WAITALL) != 2)
            return -1;
        plen = ((size_t)ext[0] << 8) | ext[1];
    } else if (plen == 127) {
        syslog(LOG_ERR, "[%s] 64-bit frame length not supported\n", TAG);
        return -1;
    }

    unsigned char mask_key[4] = { 0 };
    if (masked) {
        if (recv(fd, mask_key, 4, MSG_WAITALL) != 4)
            return -1;
    }

    if (plen >= buf_size) {
        syslog(LOG_ERR, "[%s] Frame too large: %d\n", TAG, (int)plen);
        return -1;
    }

    int received = 0;
    while ((size_t)received < plen) {
        int n = recv(fd, buf + received, plen - received, 0);
        if (n <= 0)
            return -1;
        received += n;
    }

    if (masked) {
        for (size_t i = 0; i < plen; i++) {
            ((unsigned char*)buf)[i] ^= mask_key[i % 4];
        }
    }

    buf[plen] = '\0';
    return (int)plen;
}

/* ── Per-client thread ────────────────────────────────────────── */

typedef struct {
    int fd;
} client_arg_t;

static void* client_thread(void* arg)
{
    client_arg_t ca = *(client_arg_t*)arg;
    free(arg);
    int fd = ca.fd;

    /* Peek HTTP headers to decide: REST API or WebSocket */
    char* peek_buf = malloc(2048);
    if (!peek_buf) {
        close(fd);
        return NULL;
    }
    int peek_total = 0;
    while (peek_total < 2048 - 1) {
        int n = recv(fd, peek_buf + peek_total,
            2048 - 1 - peek_total, 0);
        if (n <= 0) {
            free(peek_buf);
            close(fd);
            return NULL;
        }
        peek_total += n;
        peek_buf[peek_total] = '\0';
        if (strstr(peek_buf, "\r\n\r\n"))
            break;
    }

    /* Try REST API (config/skills/logs) */
#ifdef CONFIG_AI_AGENT_REST_API
    if (api_try_handle(fd, peek_buf, peek_total)) {
        free(peek_buf);
        /* 关之前先给协议栈时间把响应真正发出去。
         *
         * 原先这里是 SO_LINGER(0) 立即发 RST；改成 shutdown(SHUT_WR) 也不行
         * —— lwIP 在还有排队数据时关连接同样走 RST，客户端只收到前半段。
         * JSON 响应小到能塞进一个包所以一直没暴露，7 KB 的网页则每次都断在
         * 中途（浏览器表现为"页面在但脚本没跑"，永远停在连接中）。
         * 这里改成：等数据发完（PAN 上 7 KB 约 200 ms）再正常关闭走 FIN。*/
        usleep(250 * 1000);
        close(fd);
        return NULL;
    }
#endif

    /* Not REST API - proceed with WebSocket handshake using pre-read buffer */
    char chat_id[32];
    if (do_ws_handshake_ex(fd, peek_buf, peek_total,
            chat_id, sizeof(chat_id))
        != 0) {
        syslog(LOG_WARNING, "[%s] Handshake failed for fd=%d\n", TAG, fd);
        free(peek_buf);
        close(fd);
        return NULL;
    }
    free(peek_buf);

    /* Register client */
    pthread_mutex_lock(&s_clients_mtx);
    ws_client_t* client = add_client_locked(fd);
    if (!client) {
        pthread_mutex_unlock(&s_clients_mtx);
        syslog(LOG_WARNING, "[%s] Max clients reached, closing fd=%d\n", TAG, fd);
        /* Politely close */
        unsigned char close_frame[2] = { 0x88, 0x00 };
        send(fd, close_frame, 2, 0);
        close(fd);
        return NULL;
    }
    snprintf(client->chat_id, sizeof(client->chat_id), "%s", chat_id);
    pthread_mutex_unlock(&s_clients_mtx);

    /* Send connect.challenge so Nodes can identify themselves */
#ifdef CONFIG_AI_AGENT_NODE
    node_manager_send_challenge(fd);
#endif

    /* Read loop — stack-allocated so each client thread has its own buffer */
    char frame_buf[4096];
    while (s_running) {
        int n = ws_recv_frame(fd, frame_buf, sizeof(frame_buf));
        if (n < 0 && n != -2)
            break; /* -2 = ignored opcode, not an error */
        if (n == 0)
            break; /* clean close */
        if (n < 0)
            continue; /* ignored frame */

        /* Try Node protocol first — if handled, skip chat processing */
#ifdef CONFIG_AI_AGENT_NODE
        if (node_manager_handle_message(fd, frame_buf, n))
            continue;
#endif

        /* Parse JSON message */
        cJSON* root = cJSON_Parse(frame_buf);
        if (!root) {
            syslog(LOG_WARNING, "[%s] Invalid JSON from fd=%d (%d bytes): %.200s\n",
                TAG, fd, n, frame_buf);
            continue;
        }

        cJSON* type_item = cJSON_GetObjectItem(root, "type");
        cJSON* content_item = cJSON_GetObjectItem(root, "content");
        cJSON* cid_item = cJSON_GetObjectItem(root, "chat_id");

        /* Any frame carrying a chat_id re-claims that identity for this fd -
         * the phone page sends one on connect ("hello") so replies that were
         * held while its link was down are delivered right away. */
        if (cJSON_IsString(cid_item)) {
            bool claimed = false;

            pthread_mutex_lock(&s_clients_mtx);
            ws_client_t* c = find_by_fd_locked(fd);
            if (c && strcmp(c->chat_id, cid_item->valuestring) != 0) {
                strncpy(c->chat_id, cid_item->valuestring,
                    sizeof(c->chat_id) - 1);
                c->chat_id[sizeof(c->chat_id) - 1] = '\0';
                claimed = true;
            }
            pthread_mutex_unlock(&s_clients_mtx);

            if (claimed) {
                syslog(LOG_INFO, "[%s] fd=%d claims chat_id=%s\n", TAG, fd,
                    cid_item->valuestring);
                pending_flush(cid_item->valuestring, fd);
            }
        }

        if (cJSON_IsString(type_item) && strcmp(type_item->valuestring, "message") == 0 && cJSON_IsString(content_item)) {

            /* Determine/update chat_id */
            pthread_mutex_lock(&s_clients_mtx);
            ws_client_t* c = find_by_fd_locked(fd);
            if (c) {
                strncpy(chat_id, c->chat_id, sizeof(chat_id) - 1);
            }
            pthread_mutex_unlock(&s_clients_mtx);

            syslog(LOG_INFO, "[%s] WS msg from %s: %.40s\n", TAG, chat_id,
                content_item->valuestring);

#ifdef CONFIG_AI_AGENT_LVGL_UI
            /* 手机侧发来的话也要落到手表上：用户方进历史环，答案由出站
             * 分支回写桌宠页文本层（见 agent_main 的 websocket 分支） */
            lvgl_ui_channel_log(content_item->valuestring, true);
#endif

            agent_msg_t msg = { 0 };
            strncpy(msg.channel, AGENT_CHAN_WEBSOCKET, sizeof(msg.channel) - 1);
            strncpy(msg.chat_id, chat_id, sizeof(msg.chat_id) - 1);
            msg.content = strdup(content_item->valuestring);
            if (msg.content)
                message_bus_push_inbound(&msg);
        }
        cJSON_Delete(root);
    }

    /* Notify node manager before removing client */
#ifdef CONFIG_AI_AGENT_NODE
    node_manager_on_disconnect(fd);
#endif

    /* Remove and close */
    pthread_mutex_lock(&s_clients_mtx);
    remove_client_locked(fd);
    pthread_mutex_unlock(&s_clients_mtx);

    return NULL;
}

/* ── Accept loop ──────────────────────────────────────────────── */

static void* accept_thread(void* arg)
{
    (void)arg;
    while (s_running) {
        struct pollfd pfd = { .fd = s_listen_fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 500);
        if (pr <= 0)
            continue;

        struct sockaddr_in addr;
        socklen_t addr_len = sizeof(addr);
        int cfd = accept(s_listen_fd, (struct sockaddr*)&addr, &addr_len);
        if (cfd < 0)
            continue;

        client_arg_t* ca = malloc(sizeof(client_arg_t));
        if (!ca) {
            close(cfd);
            continue;
        }
        ca->fd = cfd;

        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_attr_setstacksize(&attr, AGENT_WS_CLIENT_STACK);
        if (pthread_create(&tid, &attr, client_thread, ca) != 0) {
            free(ca);
            close(cfd);
        }
        pthread_attr_destroy(&attr);
    }
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────── */

int ws_server_start(void)
{
    memset(s_clients, 0, sizeof(s_clients));

    s_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s_listen_fd < 0) {
        syslog(LOG_ERR, "[%s] socket() failed\n", TAG);
        return ERROR;
    }

    int opt = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(AGENT_WS_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };

    if (bind(s_listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        syslog(LOG_ERR, "[%s] bind() failed on port %d, errno=%d\n", TAG,
            AGENT_WS_PORT, errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        return ERROR;
    }

    if (listen(s_listen_fd, AGENT_WS_MAX_CLIENTS) < 0) {
        syslog(LOG_ERR, "[%s] listen() failed\n", TAG);
        close(s_listen_fd);
        s_listen_fd = -1;
        return ERROR;
    }

    s_running = true;

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&attr, AGENT_CLI_STACK);
    int rc = pthread_create(&tid, &attr, accept_thread, NULL);
    pthread_attr_destroy(&attr);

    if (rc != 0) {
        syslog(LOG_ERR, "[%s] Failed to create accept thread\n", TAG);
        s_running = false;
        close(s_listen_fd);
        s_listen_fd = -1;
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] WebSocket server started on port %d\n", TAG,
        AGENT_WS_PORT);
    return OK;
}

/* Build one {"type":"response",...} frame and write it. */
static int send_response_frame(int fd, const char* chat_id, const char* text,
    bool from_local)
{
    cJSON* resp = cJSON_CreateObject();
    char* json_str;
    int rc;

    if (resp == NULL) {
        return ERROR;
    }
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);
    cJSON_AddStringToObject(resp, "source", from_local ? "local" : "cloud");
    json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (json_str == NULL) {
        return ERROR;
    }
    rc = ws_send_frame(fd, json_str, strlen(json_str));
    free(json_str);
    return (rc == 0) ? OK : ERROR;
}

/* Deliver everything held for this chat_id.  Takes the lock itself and sends
 * outside it (a send on a slow link can block for a while). */
static void pending_flush(const char* chat_id, int fd)
{
    char out[WS_PENDING_MAX][WS_PENDING_LEN];
    bool out_local[WS_PENDING_MAX];
    ws_pending_t* p;
    int start;
    int n = 0;
    int i;

    pthread_mutex_lock(&s_clients_mtx);
    p = pending_slot_locked(chat_id, false);
    if (p != NULL && p->count > 0) {
        start = (p->head - p->count + WS_PENDING_MAX * 2) % WS_PENDING_MAX;
        for (i = 0; i < p->count; i++) {
            int slot = (start + i) % WS_PENDING_MAX;
            memcpy(out[n], p->text[slot], WS_PENDING_LEN);
            out[n][WS_PENDING_LEN - 1] = '\0';
            out_local[n] = p->from_local[slot];
            n++;
        }
        p->count = 0;
        p->head = 0;
    }
    pthread_mutex_unlock(&s_clients_mtx);

    for (i = 0; i < n; i++) {
        if (send_response_frame(fd, chat_id, out[i], out_local[i]) == OK) {
            syslog(LOG_INFO, "[%s] delivered held reply to %s\n", TAG, chat_id);
        }
    }
}

int ws_server_send(const char* chat_id, const char* text, bool from_local)
{
#define STACK_JSON_LEN 1600
    char json_str[STACK_JSON_LEN];
    cJSON* resp;
    int rc = -1;
    bool held = false;

    if (s_listen_fd < 0 || !s_running)
        return ERROR;

    /* Build JSON response (on the stack: this runs on the agent's outbound
     * thread, and the reply is a few hundred bytes at most). */
    resp = cJSON_CreateObject();
    if (resp == NULL)
        return ERROR;
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);
    cJSON_AddStringToObject(resp, "source", from_local ? "local" : "cloud");
    if (cJSON_PrintPreallocated(resp, json_str, STACK_JSON_LEN, 0) != 1) {
        /* Too long for the buffer: fall back to the heap. */
        char* heap = cJSON_PrintUnformatted(resp);
        cJSON_Delete(resp);
        if (heap == NULL)
            return ERROR;
        pthread_mutex_lock(&s_clients_mtx);
        ws_client_t* c2 = find_by_chat_id_locked(chat_id);
        if (c2) {
            rc = ws_send_frame(c2->fd, heap, strlen(heap));
        } else {
            pending_hold_locked(chat_id, text, from_local);
            held = true;
        }
        pthread_mutex_unlock(&s_clients_mtx);
        free(heap);
        return held ? OK : (rc == 0 ? OK : ERROR);
    }
    cJSON_Delete(resp);

    pthread_mutex_lock(&s_clients_mtx);
    ws_client_t* client = find_by_chat_id_locked(chat_id);
    if (client) {
        rc = ws_send_frame(client->fd, json_str, strlen(json_str));
        if (rc != 0) {
            syslog(
                LOG_WARNING,
                "[%s] Send failed to %s (fd will be cleaned up by client thread)\n",
                TAG, chat_id);
            /* Don't remove/close here — the client_thread owns the fd lifecycle.
             * The recv() in client_thread will fail and trigger cleanup. */
        }
    } else {
        /* Client is away (its link dropped): keep it for the next connect
         * instead of throwing the answer away. */
        pending_hold_locked(chat_id, text, from_local);
        held = true;
    }
    pthread_mutex_unlock(&s_clients_mtx);

    if (held) {
        return OK;
    }
    return (client == NULL) ? ERROR : (rc == 0 ? OK : ERROR);
#undef STACK_JSON_LEN
}

int ws_server_stop(void)
{
    s_running = false;
    if (s_listen_fd >= 0) {
        close(s_listen_fd);
        s_listen_fd = -1;
    }
    syslog(LOG_INFO, "[%s] WebSocket server stopped\n", TAG);
    return OK;
}
