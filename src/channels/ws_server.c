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
#include "infra/a2a_handler.h"
#ifdef CONFIG_AI_AGENT_NODE
#include "node/node_manager.h"
#endif
#include "agent_compat.h"
#include "agent_config.h"

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
 * Read full HTTP upgrade request, extract Sec-WebSocket-Key.
 * Returns 0 on success; sends 101 response.
 */
/**
 * WebSocket handshake. If pre_buf is non-NULL, use it as already-read
 * HTTP headers; otherwise read from fd.
 */
static int do_ws_handshake_ex(int fd, const char *pre_buf, int pre_len,
                               char *chat_id_out, size_t chat_id_size)
{
    char local_buf[1024];
    const char *buf;

    if (pre_buf) {
        buf = pre_buf;
    } else {
        int total = 0;
        while (total < (int)sizeof(local_buf) - 1) {
            int n = recv(fd, local_buf + total,
                         sizeof(local_buf) - 1 - total, 0);
            if (n <= 0) return -1;
            total += n;
            local_buf[total] = '\0';
            if (strstr(local_buf, "\r\n\r\n")) break;
        }
        buf = local_buf;
    }

    /* Extract Sec-WebSocket-Key */
    const char *key_hdr = strcasestr(buf, "\r\nSec-WebSocket-Key: ");
    if (!key_hdr) {
        syslog(LOG_WARNING, "[%s] No Sec-WebSocket-Key\n", TAG);
        return -1;
    }
    key_hdr += 21;
    const char *eol = strstr(key_hdr, "\r\n");
    if (!eol)
        return -1;

    char ws_key[128] = { 0 };
    size_t klen = (size_t)(eol - key_hdr);
    if (klen >= sizeof(ws_key))
        return -1;
    memcpy(ws_key, key_hdr, klen);
    ws_key[klen] = '\0';

    /* Compute accept */
    char accept[64] = { 0 };
    if (make_accept_key(ws_key, accept, sizeof(accept)) < 0)
        return -1;

    /* Send 101 */
    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n\r\n",
        accept);
    if (send(fd, resp, rlen, 0) != rlen)
        return -1;

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

    /* Peek HTTP headers to decide: A2A HTTP or WebSocket upgrade */
    char peek_buf[2048];
    int peek_total = 0;
    while (peek_total < (int)sizeof(peek_buf) - 1) {
        int n = recv(fd, peek_buf + peek_total,
                     sizeof(peek_buf) - 1 - peek_total, 0);
        if (n <= 0) { close(fd); return NULL; }
        peek_total += n;
        peek_buf[peek_total] = '\0';
        if (strstr(peek_buf, "\r\n\r\n")) break;
    }

    /* Try A2A HTTP routes first */
    if (a2a_try_handle(fd, peek_buf, peek_total)) {
        close(fd);
        return NULL;
    }

    /* Not A2A — proceed with WebSocket handshake */
    char chat_id[32];
    if (do_ws_handshake_ex(fd, peek_buf, peek_total,
                            chat_id, sizeof(chat_id)) != 0) {
        syslog(LOG_WARNING, "[%s] Handshake failed for fd=%d\n", TAG, fd);
        close(fd);
        return NULL;
    }

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

        if (cJSON_IsString(type_item) && strcmp(type_item->valuestring, "message") == 0 && cJSON_IsString(content_item)) {

            /* Determine/update chat_id */
            cJSON* cid_item = cJSON_GetObjectItem(root, "chat_id");
            pthread_mutex_lock(&s_clients_mtx);
            ws_client_t* c = find_by_fd_locked(fd);
            if (c) {
                if (cJSON_IsString(cid_item)) {
                    strncpy(c->chat_id, cid_item->valuestring, sizeof(c->chat_id) - 1);
                }
                strncpy(chat_id, c->chat_id, sizeof(chat_id) - 1);
            }
            pthread_mutex_unlock(&s_clients_mtx);

            syslog(LOG_INFO, "[%s] WS msg from %s: %.40s\n", TAG, chat_id,
                content_item->valuestring);

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

int ws_server_send(const char* chat_id, const char* text)
{
    if (s_listen_fd < 0 || !s_running)
        return ERROR;

    /* Build JSON response */
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);
    char* json_str = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (!json_str)
        return ERROR;

    pthread_mutex_lock(&s_clients_mtx);
    ws_client_t* client = find_by_chat_id_locked(chat_id);
    int rc = -1;
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
        syslog(LOG_WARNING, "[%s] No WS client for chat_id=%s\n", TAG, chat_id);
    }
    pthread_mutex_unlock(&s_clients_mtx);

    free(json_str);
    return (client == NULL) ? ERROR : (rc == 0 ? OK : ERROR);
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
