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
#include "channels/mic_stream.h"
#include "core/message_bus.h"
#ifdef CONFIG_AI_AGENT_NODE
#include "node/node_manager.h"
#endif
#include "agent_compat.h"
#include "agent_config.h"
#ifdef CONFIG_AI_AGENT_REST_API
#include "infra/api_handler.h"
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

/* Size of the per-client scratch used to coalesce a frame's header and
 * payload into a single send(). 2 KB covers an audio frame (the stream caps
 * at 1024 samples) with room to spare; anything larger falls back to a heap
 * buffer. It is per-client rather than shared because two clients are
 * written concurrently by different threads now that sending no longer
 * serialises globally. */
#define WS_FRAME_SCRATCH 2048

typedef struct {
    int fd;
    char chat_id[32];
    bool active;

    /* Serialises frames on THIS client's socket.
     *
     * It used to be one mutex for every client, held across the send itself.
     * A send is a blocked send for as long as the frame takes to shift over
     * a ~6 KB/s link, so one peer that stopped reading -- or just a busy one
     * -- parked that mutex inside send() and starved every other writer:
     * the microphone stream, agent replies, and the pong that answers the
     * far end's keepalive. Losing the pong drops the session, and losing the
     * stream stalls the demo, both caused by a third party doing nothing
     * wrong. Per-client means a slow peer only slows itself.
     *
     * Lock order is always table-lock then client-lock; that is the only
     * order taken anywhere, so it cannot deadlock.
     */
    pthread_mutex_t tx_mtx;
    unsigned char   scratch[WS_FRAME_SCRATCH];
} ws_client_t;

static ws_client_t s_clients[AGENT_WS_MAX_CLIENTS];
static pthread_mutex_t s_clients_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool s_tx_mtx_ready = false;

static int s_listen_fd = -1;
static volatile bool s_running = false;

/* ── Client helpers ───────────────────────────────────────────── */

/* Call with s_clients_mtx held. */
static ws_client_t* find_by_fd_locked(int fd)
{
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && s_clients[i].fd == fd)
            return &s_clients[i];
    }
    return NULL;
}

/* Call with s_clients_mtx held. */
static ws_client_t* find_by_chat_id_locked(const char* chat_id)
{
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
        if (s_clients[i].active && strcmp(s_clients[i].chat_id, chat_id) == 0)
            return &s_clients[i];
    }
    return NULL;
}

/* Hold a client's transmit lock, or NULL if it is not there.
 *
 * The table lock is taken only long enough to find the entry and claim its
 * transmit lock. Holding it across the send instead is what let one slow
 * peer stall the whole server; holding it only for the lookup means the
 * table can still be read and written while a frame is going out -- which
 * is why the transmit lock has to be claimed here, before the table lock is
 * released: otherwise the entry could be removed and its fd closed and
 * reused underneath the sender.
 */
static ws_client_t* lock_client_for_send(const char* chat_id)
{
    ws_client_t* client;

    pthread_mutex_lock(&s_clients_mtx);
    client = find_by_chat_id_locked(chat_id);
    if (client != NULL) {
        pthread_mutex_lock(&client->tx_mtx);
    }
    pthread_mutex_unlock(&s_clients_mtx);

    return client;
}

static ws_client_t* lock_client_by_fd(int fd)
{
    ws_client_t* client;

    pthread_mutex_lock(&s_clients_mtx);
    client = find_by_fd_locked(fd);
    if (client != NULL) {
        pthread_mutex_lock(&client->tx_mtx);
    }
    pthread_mutex_unlock(&s_clients_mtx);

    return client;
}

static void unlock_client(ws_client_t* client)
{
    if (client != NULL) {
        pthread_mutex_unlock(&client->tx_mtx);
    }
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

            /* Wait for a send that is already in flight before closing, or
             * the fd could be closed and handed to something else while the
             * sender still writes to it. Same lock order as the send path. */
            pthread_mutex_lock(&s_clients[i].tx_mtx);
            s_clients[i].active = false;
            s_clients[i].fd = -1; /* invalidate fd before closing */
            pthread_mutex_unlock(&s_clients[i].tx_mtx);

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
static int do_ws_handshake_ex(int fd, const char* buf, int buf_len,
    char* chat_id_out, size_t chat_id_size)
{

    /* Extract Sec-WebSocket-Key */
    char* key_hdr = strcasestr(buf, "\r\nSec-WebSocket-Key: ");
    if (!key_hdr) {
        syslog(LOG_WARNING, "[%s] No Sec-WebSocket-Key\n", TAG);
        return -1;
    }
    key_hdr += 21;
    char* eol = strstr(key_hdr, "\r\n");
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

    /* Default chat_id from fd (may be overridden by first message) */
    snprintf(chat_id_out, chat_id_size, "ws_%d", fd);
    return 0;
}

/* ── WS frame encode/decode ───────────────────────────────────── */

/* Opcodes this server emits. Incoming frames are decoded separately. */
#define WS_OPCODE_TEXT   0x1
#define WS_OPCODE_BINARY 0x2

/**
 * Send one unfragmented frame (server → client, no masking).
 * Caller must hold the client's tx_mtx, which also covers its scratch.
 */
static int ws_send_frame_op(ws_client_t* client, const void* payload,
    size_t len, int opcode)
{
    unsigned char hdr[10];
    int hdr_len = 0;
    int fd = client->fd;

    hdr[0] = (unsigned char)(0x80 | opcode); /* FIN + opcode */
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

    /* Header and payload go out in a single send().
     *
     * Emitting them separately makes Nagle hold the payload back until the
     * header is acknowledged, while the peer's delayed ACK waits for a second
     * segment before acknowledging -- the two together stall each frame for
     * roughly the delayed-ACK timer. On a microphone stream that is fatal:
     * at 100 ms per frame it throttles delivery to about a third of the
     * capture rate, and the samples the reader never gets to are silently
     * overwritten by the DMA.
     */
    if (hdr_len + len <= sizeof(client->scratch))
      {
        memcpy(client->scratch, hdr, hdr_len);
        memcpy(client->scratch + hdr_len, payload, len);
        if (send(fd, client->scratch, hdr_len + len, 0) != hdr_len + (int)len)
          {
            return -1;
          }

        return 0;
      }

    /* Larger frames: still one send(), from the heap. Never two sends -- see
     * the note above about interleaving with other writers. */
    unsigned char* big = malloc(hdr_len + len);
    if (big == NULL)
      {
        syslog(LOG_ERR, "[%s] out of memory for %d byte frame\n", TAG,
            (int)(hdr_len + len));
        return -1;
      }

    memcpy(big, hdr, hdr_len);
    memcpy(big + hdr_len, payload, len);
    int rc = (send(fd, big, hdr_len + len, 0) == hdr_len + (int)len) ? 0 : -1;
    free(big);
    return rc;
}

/**
 * Send a text frame. Caller must hold the client's tx_mtx.
 */
static int ws_send_frame(ws_client_t* client, const char* payload, size_t len)
{
    return ws_send_frame_op(client, payload, len, WS_OPCODE_TEXT);
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

        /* Reply with pong if it was a ping.
         *
         * Assembled into one buffer and sent holding this client's transmit
         * lock: this runs on the client thread while other threads (the
         * microphone stream, agent replies) write to the same socket, and a
         * control frame that lands between another frame's header and its
         * payload desynchronises the peer's parser. Coalescing also avoids
         * the same Nagle/delayed-ACK stall that splitting a frame across two
         * sends causes.
         *
         * This is the send that used to be the most expensive to delay: the
         * far end treats a late pong as a dead connection and drops a
         * session that is working perfectly.
         */
        if (opcode == 0x9) {
            unsigned char pong[2 + 125];
            size_t n = 0;

            pong[n++] = 0x8A;
            pong[n++] = (unsigned char)plen;
            if (plen > 0) {
                memcpy(pong + n, ctrl_payload, plen);
                n += plen;
            }

            ws_client_t* c = lock_client_by_fd(fd);
            if (c != NULL) {
                send(fd, pong, n, 0);
                unlock_client(c);
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

/* How long a send to one client may block before it is treated as failed.
 *
 * A healthy peer drains in milliseconds -- the worst real frame is a few KB
 * over a ~6 KB/s link, so a few hundred of them -- and anything approaching
 * this is a peer that has stopped reading or vanished without the socket
 * noticing. Without a bound, send() parks for as long as the stack takes to
 * give up, and a blocked send sits inside the network lock: one stalled peer
 * then stops the microphone stream and every other socket on the device,
 * which is exactly what a load test with a silent client reproduced. With
 * it, the frame fails, the client is dropped, and the rest carries on.
 */
#define WS_SEND_TIMEOUT_S 5

static void* client_thread(void* arg)
{
    client_arg_t ca = *(client_arg_t*)arg;
    free(arg);
    int fd = ca.fd;

    {
        struct timeval tv;
        tv.tv_sec = WS_SEND_TIMEOUT_S;
        tv.tv_usec = 0;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
            syslog(LOG_WARNING, "[%s] fd=%d: SO_SNDTIMEO not set (%d)\n",
                TAG, fd, errno);
        }
    }

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

    syslog(LOG_INFO, "[%s] fd=%d: %d bytes, first line: %.60s\n", TAG, fd,
        peek_total, peek_buf);

    /* Try REST API (config/skills/logs) */
#ifdef CONFIG_AI_AGENT_REST_API
    if (api_try_handle(fd, peek_buf, peek_total)) {
        free(peek_buf);
        /* Graceful close only. SO_LINGER with l_linger=0 here used to send
         * an RST that discarded the response still in the send buffer,
         * so REST clients saw "Connection reset by peer" and no body. */
        shutdown(fd, SHUT_WR);
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
        /* Answer plain-HTTP callers with a 400 instead of a silent close,
         * so the client sees a real response instead of a bare FIN/RST. */
        static const char bad[] =
            "HTTP/1.1 400 Bad Request\r\n"
            "Content-Type: text/plain\r\n"
            "Content-Length: 20\r\n"
            "Connection: close\r\n\r\n"
            "not a WebSocket peer\n";
        send(fd, bad, sizeof(bad) - 1, 0);
        shutdown(fd, SHUT_WR);
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

        /* Microphone streaming control. These frames carry no content; they
         * switch this client's capture stream on and off. The reply, if any,
         * arrives asynchronously as binary PCM frames plus normal agent
         * responses.
         */
        if (cJSON_IsString(type_item)
            && (strcmp(type_item->valuestring, "mic_start") == 0
                || strcmp(type_item->valuestring, "mic_stop") == 0)) {
            bool want = (strcmp(type_item->valuestring, "mic_start") == 0);
            cJSON* n_item = cJSON_GetObjectItem(root, "samples");
            int samples = cJSON_IsNumber(n_item) ? (int)n_item->valuedouble : 0;
            syslog(LOG_INFO, "[%s] %s request from %s\n", TAG,
                type_item->valuestring, chat_id);
            cJSON_Delete(root);
            if (want)
                mic_stream_start(chat_id, samples);
            else
                mic_stream_stop();
            continue;
        }

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

        syslog(LOG_INFO, "[%s] accepted fd=%d from %s:%d\n", TAG, cfd,
            inet_ntoa(addr.sin_addr), ntohs(addr.sin_port));

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
    /* The transmit mutexes live in the table, so the table cannot simply be
     * memset() on a restart -- that would zero live mutexes. Re-initialising
     * them is equally wrong, so first time round zero the table and create
     * them; after that, clear only the fields that describe the connection. */
    if (!s_tx_mtx_ready) {
        memset(s_clients, 0, sizeof(s_clients));
        for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
            pthread_mutex_init(&s_clients[i].tx_mtx, NULL);
        }
        s_tx_mtx_ready = true;
    } else {
        for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
            s_clients[i].fd = -1;
            s_clients[i].active = false;
            s_clients[i].chat_id[0] = '\0';
        }
    }

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

    ws_client_t* client = lock_client_for_send(chat_id);
    int rc = -1;
    if (client) {
        rc = ws_send_frame(client, json_str, strlen(json_str));
        if (rc != 0) {
            syslog(
                LOG_WARNING,
                "[%s] Send failed to %s (fd will be cleaned up by client thread)\n",
                TAG, chat_id);
            /* Don't remove/close here — the client_thread owns the fd lifecycle.
             * The recv() in client_thread will fail and trigger cleanup. */
        }
        unlock_client(client);
    } else {
        syslog(LOG_WARNING, "[%s] No WS client for chat_id=%s\n", TAG, chat_id);
    }

    free(json_str);
    return (client == NULL) ? ERROR : (rc == 0 ? OK : ERROR);
}

/**
 * Send a pre-built JSON control frame to a client.
 *
 * Unlike ws_server_send() this does not wrap the payload in a chat response
 * envelope, which is what control and status frames need.
 */
int ws_server_send_json(const char* chat_id, const char* json)
{
    if (s_listen_fd < 0 || !s_running || json == NULL)
        return ERROR;

    ws_client_t* client = lock_client_for_send(chat_id);
    int rc = -1;
    if (client) {
        rc = ws_send_frame(client, json, strlen(json));
        unlock_client(client);
    }

    return (client == NULL) ? ERROR : (rc == 0 ? OK : ERROR);
}

/**
 * Send a control frame to every connected client.
 *
 * For events that originate on the device and belong to whichever host
 * happens to be attached, rather than to one named peer: the UI's
 * push-to-talk button is the case this exists for, and the button has no
 * way to know which chat_id the host registered under -- it may not even
 * be connected yet when the screen is built.
 */
int ws_server_broadcast_json(const char* json)
{
    int sent = 0;

    if (s_listen_fd < 0 || !s_running || json == NULL)
        return ERROR;

    /* One client at a time, each claimed under the table lock and released
     * before moving on: a peer that is not reading must not hold up the
     * broadcast to the others, which is the whole reason sends no longer
     * share one mutex. */
    for (int i = 0; i < AGENT_WS_MAX_CLIENTS; i++) {
        int fd;

        pthread_mutex_lock(&s_clients_mtx);
        if (!s_clients[i].active) {
            pthread_mutex_unlock(&s_clients_mtx);
            continue;
        }
        pthread_mutex_lock(&s_clients[i].tx_mtx);
        fd = s_clients[i].fd;
        pthread_mutex_unlock(&s_clients_mtx);

        if (fd >= 0 && ws_send_frame(&s_clients[i], json, strlen(json)) == 0)
            sent++;

        pthread_mutex_unlock(&s_clients[i].tx_mtx);
    }

    syslog(LOG_INFO, "[%s] broadcast %s to %d client(s)\n", TAG, json, sent);
    return sent > 0 ? OK : ERROR;
}

/**
 * Push one binary frame to a client.
 *
 * Used for raw stream payloads (currently microphone PCM) that would be
 * wasteful and fragile to carry as base64 inside JSON. Frames larger than
 * 65535 bytes are rejected by the encoder, so callers must chunk.
 */
int ws_server_send_binary(const char* chat_id, const void* data, size_t len)
{
    if (s_listen_fd < 0 || !s_running || data == NULL || len == 0)
        return ERROR;

    ws_client_t* client = lock_client_for_send(chat_id);
    int rc = -1;
    if (client) {
        rc = ws_send_frame_op(client, data, len, WS_OPCODE_BINARY);
        unlock_client(client);
    }

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
