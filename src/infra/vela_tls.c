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

#include "infra/vela_tls.h"
#include "infra/http_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"

#ifdef CONFIG_AI_AGENT_NET_RPMSG
#include "network/network_manager.h"
#endif

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* POSIX networking */
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

/* mbedTLS */
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

static int simple_entropy_func(void* data, unsigned char* output, size_t len)
{
    (void)data;
    if (agent_secure_random(output, len) == 0) {
        return 0;
    }
    /* No fallback — cryptographic entropy is mandatory for TLS.
     * Returning an error forces the TLS handshake to fail safely
     * rather than proceeding with predictable key material. */
    syslog(LOG_ERR, "[vela_tls] CRITICAL: No secure entropy source available\n");
    return -1;  /* Generic error - TLS handshake will fail safely */
}

static const char* TAG = "vela_tls";

/* Upper bound (seconds) on a single TLS handshake.  The socket is blocking
 * with SO_RCVTIMEO, so a WANT_READ retry just blocks again for another full
 * timeout; capping the handshake lets tls_ctx_connect_retry() actually recover
 * from a half-open MiMo edge instead of stalling forever. */
#define AGENT_TLS_HANDSHAKE_TIMEOUT_SEC 10

/* ── Chunked transfer decoding ───────────────────────────────── */

/**
 * Decode chunked transfer encoding in-place.
 * Format: <hex-size>\r\n<data>\r\n ... 0\r\n\r\n
 * Returns the decoded length.
 */
static size_t decode_chunked(char* buf, size_t len)
{
    char* src = buf;
    char* end = buf + len;
    char* dst = buf;

    while (src < end) {
        /* Find end of chunk-size line */
        char* crlf = (char*)memmem(src, (size_t)(end - src), "\r\n", 2);
        if (!crlf)
            break;

        /* Parse hex chunk size */
        char* endptr;
        long chunk_sz = strtol(src, &endptr, 16);

        /* Validate: endptr should reach the CRLF (skip spaces) */
        while (endptr < crlf && *endptr == ' ')
            endptr++;

        if (endptr != crlf || chunk_sz < 0 || chunk_sz > (long)(end - crlf - 2))
            break;  /* malformed or oversized chunk header */

        if (chunk_sz == 0)
            break;  /* final chunk */

        src = crlf + 2;  /* skip past chunk-size CRLF */

        /* Clamp to available data */
        if (src + chunk_sz > end)
            chunk_sz = (long)(end - src);

        memmove(dst, src, (size_t)chunk_sz);
        dst += chunk_sz;
        src += chunk_sz;

        /* Skip trailing CRLF after chunk data */
        if (src + 2 <= end && src[0] == '\r' && src[1] == '\n')
            src += 2;
    }

    return (size_t)(dst - buf);
}

/* Return 1 if the buffered chunked-encoded body [buf, len) has received its
 * terminal final chunk ("0" size line), meaning the whole response body is
 * here and no further reads are needed.  Without this, tls_read_response()
 * keeps calling mbedtls_ssl_read() on a keep-alive connection that never
 * closes, stalling for the server's idle timeout (or the full SO_RCVTIMEO)
 * before the chunked body is finally decoded.  Chunk payloads are base64 here,
 * so CR/LF bytes can only belong to chunk framing, never to payload data. */
static int chunked_body_complete(const char* buf, size_t len)
{
    const char* p = buf;
    const char* end = buf + len;

    while (p < end) {
        const char* crlf = (const char*)memmem(p, (size_t)(end - p), "\r\n", 2);
        if (!crlf || crlf == p) {
            return 0; /* incomplete or empty size line */
        }

        size_t slen = (size_t)(crlf - p);
        if (slen >= 16) {
            return 0; /* oversized size line — malformed */
        }

        char tmp[16];
        memcpy(tmp, p, slen);
        tmp[slen] = '\0';

        char* eptr = NULL;
        long sz = strtol(tmp, &eptr, 16);
        while (eptr && *eptr == ' ') {
            eptr++;
        }
        if (!eptr || *eptr != '\0' || sz < 0) {
            return 0; /* malformed size line */
        }

        if (sz == 0) {
            return 1; /* final chunk header seen — body complete */
        }

        p = crlf + 2;                 /* start of chunk payload */
        if ((size_t)(end - p) < (size_t)sz + 2) {
            return 0;                 /* payload / trailing CRLF not arrived */
        }
        p += (size_t)sz + 2;          /* skip payload + trailing CRLF */
    }

    return 0;
}

/* ── TLS context ─────────────────────────────────────────────── */

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
} tls_ctx_t;

static void tls_ctx_free(tls_ctx_t* ctx)
{
    mbedtls_ssl_close_notify(&ctx->ssl);
    mbedtls_net_free(&ctx->net);
    mbedtls_ssl_free(&ctx->ssl);
    mbedtls_ssl_config_free(&ctx->cfg);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);

#ifdef CONFIG_AI_AGENT_NET_RPMSG
    network_release_resource();
#endif
}

/* ── Persistent connection pool (keep-alive) ─────────────────── */
/* Keeps one live TLS connection per host:port so repeated calls to
 * the same endpoint (Feishu REST, LLM API) skip the TLS handshake. */

#ifdef CONFIG_AI_AGENT_TLS_CONN_POOL_SIZE
#define CONN_POOL_SIZE CONFIG_AI_AGENT_TLS_CONN_POOL_SIZE
#else
#define CONN_POOL_SIZE 2
#endif

typedef struct {
    tls_ctx_t ctx;
    char host[128];
    char port[8];
    bool in_use; /* locked by a request */
    bool valid; /* connection is alive */
} conn_slot_t;

static conn_slot_t s_pool[CONN_POOL_SIZE];
static pthread_mutex_t s_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/* Acquire a slot for host:port.  Returns a locked, connected ctx or NULL. */
static conn_slot_t* pool_acquire(const char* host, const char* port)
{
    pthread_mutex_lock(&s_pool_lock);

    /* 1. Find an existing valid slot for this host:port */
    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        conn_slot_t* s = &s_pool[i];
        if (s->valid && !s->in_use && strcmp(s->host, host) == 0 && strcmp(s->port, port) == 0) {
            s->in_use = true;
            pthread_mutex_unlock(&s_pool_lock);
            return s;
        }
    }

    /* 2. Find an empty slot */
    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        if (!s_pool[i].valid && !s_pool[i].in_use) {
            s_pool[i].in_use = true;
            pthread_mutex_unlock(&s_pool_lock);
            return &s_pool[i];
        }
    }

    /* 3. Evict the first non-in-use slot */
    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        if (!s_pool[i].in_use) {
            tls_ctx_free(&s_pool[i].ctx);
            s_pool[i].valid = false;
            s_pool[i].in_use = true;
            pthread_mutex_unlock(&s_pool_lock);
            return &s_pool[i];
        }
    }

    pthread_mutex_unlock(&s_pool_lock);
    return NULL; /* all slots busy — caller falls back to ephemeral */
}

/* Return a slot to the pool.  keep=true means the connection is still alive. */
static void pool_release(conn_slot_t* s, const char* host, const char* port, bool keep)
{
    pthread_mutex_lock(&s_pool_lock);
    if (keep) {
        strncpy(s->host, host, sizeof(s->host) - 1);
        strncpy(s->port, port, sizeof(s->port) - 1);
        s->valid = true;
    } else {
        tls_ctx_free(&s->ctx);
        s->valid = false;
        s->host[0] = '\0';
    }
    s->in_use = false;
    pthread_mutex_unlock(&s_pool_lock);
}

void vela_tls_pool_cleanup(void)
{
    pthread_mutex_lock(&s_pool_lock);
    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        if (s_pool[i].in_use) {
            syslog(LOG_WARNING,
                "[vela_tls] Pool slot %d still in use at shutdown, skipping\n", i);
            continue;
        }
        if (s_pool[i].valid) {
            tls_ctx_free(&s_pool[i].ctx);
            s_pool[i].valid = false;
        }
        s_pool[i].host[0] = '\0';
    }
    pthread_mutex_unlock(&s_pool_lock);
}

/* Configure the connected socket: blocking mode + SO_RCVTIMEO (recv_timeout_sec)
 * (+ SO_SNDTIMEO under RPMSG).  Kept separate from the connect so it can be
 * re-applied per resolved IP.  Forces blocking instead of select()/poll(),
 * which returns MBEDTLS_ERR_NET_POLL_FAILED (-0x0047) on NuttX/QEMU. */
static void tls_configure_socket(tls_ctx_t* ctx, int recv_timeout_sec)
{
    mbedtls_net_set_block(&ctx->net);
    if (ctx->net.fd < 0) {
        return;
    }

    struct timeval tv = { .tv_sec = recv_timeout_sec, .tv_usec = 0 };
    setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

#ifdef CONFIG_AI_AGENT_NET_RPMSG
    {
        int timeout_sec = network_get_connect_timeout();
        struct timeval ctv = { .tv_sec = timeout_sec, .tv_usec = 0 };
        setsockopt(ctx->net.fd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));
    }
#endif
}

/* Run the TLS handshake, bounding total elapsed time to
 * AGENT_TLS_HANDSHAKE_TIMEOUT_SEC.  Returns 0 on success, otherwise
 * VELA_TLS_ERR_HANDSHAKE. */
static int tls_do_handshake(tls_ctx_t* ctx)
{
    int ret;
    struct timespec hs0;
    clock_gettime(CLOCK_MONOTONIC, &hs0);

    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
#if defined(MBEDTLS_ERROR_C)
            char err_buf[128];
            mbedtls_strerror(ret, err_buf, sizeof(err_buf));
            syslog(LOG_ERR, "[%s] ssl_handshake ret=-0x%04x: %s\n", TAG, -ret, err_buf);
#else
            syslog(LOG_ERR, "[%s] ssl_handshake ret=-0x%04x\n", TAG, -ret);
#endif
            if (ret == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) {
                syslog(LOG_ERR, "[%s] Server sent fatal alert message\n", TAG);
            }
            return VELA_TLS_ERR_HANDSHAKE;
        }

        struct timespec hs1;
        clock_gettime(CLOCK_MONOTONIC, &hs1);
        if (hs1.tv_sec - hs0.tv_sec >= AGENT_TLS_HANDSHAKE_TIMEOUT_SEC) {
            syslog(LOG_ERR, "[%s] ssl_handshake timed out (>%ds)\n",
                TAG, AGENT_TLS_HANDSHAKE_TIMEOUT_SEC);
            return VELA_TLS_ERR_HANDSHAKE;
        }
    }

    return 0;
}

static int tls_ctx_connect(tls_ctx_t* ctx, const char* host, const char* port)
{
    int ret;

    /* Replicate mbedtls_net_connect()'s net_prepare(): ignore SIGPIPE so a
     * write to a peer-closed socket returns EPIPE instead of terminating us.
     * (Our manual connect below bypasses net_prepare.) */
    signal(SIGPIPE, SIG_IGN);

    mbedtls_ssl_init(&ctx->ssl);
    mbedtls_ssl_config_init(&ctx->cfg);
    mbedtls_net_init(&ctx->net);
    mbedtls_ctr_drbg_init(&ctx->ctr_drbg);

    /* Seed RNG directly with our robust function */
    const char* pers = "vela_tls";
    if ((ret = mbedtls_ctr_drbg_seed(&ctx->ctr_drbg, simple_entropy_func, NULL,
             (const unsigned char*)pers,
             strlen(pers)))
        != 0) {
        syslog(LOG_ERR, "[%s] ctr_drbg_seed ret=0x%x\n", TAG, -ret);
        return VELA_TLS_ERR_HANDSHAKE;
    }

    /* Check system time — crucial for TLS certificate validation. */
    time_t now = time(NULL);
    syslog(LOG_DEBUG, "[%s] Handshake start: Host=%s, UNIX=%ld\n", TAG, host, (long)now);

    if (now < 1704067200) { /* Jan 1 2024 */
        syslog(LOG_WARNING, "[%s] Clock too old, forcing to 2026\n", TAG);
        struct timespec ts = { .tv_sec = 1772275200, .tv_nsec = 0 };
        clock_settime(CLOCK_REALTIME, &ts);
    }

    /* TCP connect — use HTTP CONNECT proxy if configured */
#ifdef CONFIG_AI_AGENT_NET_RPMSG
    {
        int res_ret = network_acquire_resource(network_get_connect_timeout() * 1000);
        if (res_ret != 0) {
            syslog(LOG_ERR, "[%s] Resource acquire failed: %d\n", TAG, res_ret);
            return VELA_TLS_ERR_CONNECT;
        }
    }
#endif

    /* SSL config + setup + hostname + bio are IP-independent — do them once,
     * before the connect loop, so we can retry the handshake across multiple
     * resolved addresses (the MiMo edge resolves to several IPs, some of which
     * serve an Ed25519 leaf cert this mbedtls build cannot parse). */
    if ((ret = mbedtls_ssl_config_defaults(&ctx->cfg,
             MBEDTLS_SSL_IS_CLIENT,
             MBEDTLS_SSL_TRANSPORT_STREAM,
             MBEDTLS_SSL_PRESET_DEFAULT))
        != 0) {
        syslog(LOG_ERR, "[%s] ssl_config_defaults ret=0x%x\n", TAG, -ret);
        return VELA_TLS_ERR_HANDSHAKE;
    }

    /* Pin the TLS version range to what is actually compiled in.
     * Setting max to MBEDTLS_SSL_VERSION_TLS1_3 when MBEDTLS_SSL_PROTO_TLS1_3
     * is not defined causes mbedtls_ssl_setup() to return BAD_CONFIG (-0x5e80).
     * Use TLS 1.3 as the ceiling only when the library was built with TLS 1.3
     * support; otherwise cap at TLS 1.2. */
    mbedtls_ssl_conf_min_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#if defined(MBEDTLS_SSL_PROTO_TLS1_3)
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_3);
#else
    mbedtls_ssl_conf_max_tls_version(&ctx->cfg, MBEDTLS_SSL_VERSION_TLS1_2);
#endif

    /* Advertise ALPN so Cloudflare/OpenAI servers don't silently reject us.
     * Modern HTTPS servers send a fatal TLS alert or close the connection
     * when no ALPN extension is present in the ClientHello.
     * MBEDTLS_SSL_ALPN must be enabled in the build (it is). */
#if defined(MBEDTLS_SSL_ALPN)
    static const char* alpn_protos[] = { "http/1.1", NULL };
    if ((ret = mbedtls_ssl_conf_alpn_protocols(&ctx->cfg, alpn_protos)) != 0) {
        syslog(LOG_WARNING, "[%s] Failed to set ALPN protocols: -0x%04x (non-fatal)\n", TAG, -ret);
    }
#endif

    /* Skip full chain verification — keeps portability without CA bundle */
    mbedtls_ssl_conf_authmode(&ctx->cfg, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_rng(&ctx->cfg, mbedtls_ctr_drbg_random, &ctx->ctr_drbg);

    if ((ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg)) != 0) {
        syslog(LOG_ERR, "[%s] ssl_setup ret=0x%x\n", TAG, -ret);
        return VELA_TLS_ERR_HANDSHAKE;
    }

    if ((ret = mbedtls_ssl_set_hostname(&ctx->ssl, host)) != 0) {
        syslog(LOG_ERR, "[%s] ssl_set_hostname ret=0x%x\n", TAG, -ret);
        return VELA_TLS_ERR_HANDSHAKE;
    }

    /* Use blocking recv — no select()/poll().  The read timeout is enforced by
     * the SO_RCVTIMEO socket option set in tls_configure_socket(). */
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net,
        mbedtls_net_send, mbedtls_net_recv, NULL);

    int connected = 0;

    /* TCP connect — use HTTP CONNECT proxy if configured */
    if (http_proxy_is_enabled()) {
        int tunnel_fd = proxy_open_tunnel(host, atoi(port), 30000);
        if (tunnel_fd < 0) {
            syslog(LOG_ERR, "[%s] proxy tunnel to %s:%s failed\n",
                TAG, host, port);
            return VELA_TLS_ERR_CONNECT;
        }
        ctx->net.fd = tunnel_fd;
        syslog(LOG_INFO, "[%s] Using proxy tunnel fd=%d for %s:%s\n",
            TAG, tunnel_fd, host, port);

        tls_configure_socket(ctx, AGENT_TLS_HANDSHAKE_TIMEOUT_SEC);
        mbedtls_ssl_session_reset(&ctx->ssl);
        if (tls_do_handshake(ctx) == 0) {
            connected = 1;
        }
    } else {
        /* Manual connect (instead of mbedtls_net_connect) so we can set
         * SO_SNDTIMEO on the socket before connect().  A half-open peer
         * (flaky MiMo edge) would otherwise block the caller for the full
         * TCP SYN retry window, which has no timeout on this build.
         *
         * The endpoint resolves to multiple IPs; some intermittently serve an
         * Ed25519 leaf cert this mbedtls build cannot parse (or send invalid
         * TLS records).  Iterate ALL addresses and run the handshake on each
         * until one succeeds, instead of retrying the same first IP. */
        struct addrinfo hints;
        struct addrinfo* addr_list = NULL;
        struct addrinfo* cur;

        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = 0;

        if (getaddrinfo(host, port, &hints, &addr_list) != 0) {
            syslog(LOG_ERR, "[%s] getaddrinfo %s:%s failed\n",
                TAG, host, port);
            return VELA_TLS_ERR_CONNECT;
        }

        for (cur = addr_list; cur != NULL && !connected; cur = cur->ai_next) {
            int fd = (int)socket(cur->ai_family, cur->ai_socktype,
                cur->ai_protocol);
            if (fd < 0) {
                continue;
            }

            struct timeval ctv = { .tv_sec = AGENT_LLM_SOCKET_TIMEOUT_SEC,
                .tv_usec = 0 };
            setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));

            if (connect(fd, cur->ai_addr, cur->ai_addrlen) != 0) {
                close(fd);
                continue;
            }

            char ipbuf[64] = "?";
            if (cur->ai_family == AF_INET) {
                inet_ntop(AF_INET,
                    &((struct sockaddr_in*)cur->ai_addr)->sin_addr,
                    ipbuf, sizeof(ipbuf));
            } else if (cur->ai_family == AF_INET6) {
                inet_ntop(AF_INET6,
                    &((struct sockaddr_in6*)cur->ai_addr)->sin6_addr,
                    ipbuf, sizeof(ipbuf));
            }

            ctx->net.fd = fd;
            syslog(LOG_INFO, "[%s] TCP connected to %s:%s (fd=%d, ip=%s)\n",
                TAG, host, port, fd, ipbuf);

            tls_configure_socket(ctx, AGENT_TLS_HANDSHAKE_TIMEOUT_SEC);
            mbedtls_ssl_session_reset(&ctx->ssl);
            if (tls_do_handshake(ctx) == 0) {
                connected = 1;
                break;
            }

            syslog(LOG_WARNING,
                "[%s] handshake failed on ip=%s, trying next address\n",
                TAG, ipbuf);
            close(fd);
            ctx->net.fd = -1;
        }

        freeaddrinfo(addr_list);
    }

    if (!connected) {
        syslog(LOG_ERR, "[%s] connect/handshake %s:%s failed on all addresses\n",
            TAG, host, port);
        return VELA_TLS_ERR_HANDSHAKE;
    }

    /* Restore the long read timeout for the response body. */
#ifdef CONFIG_AI_AGENT_NET_RPMSG
    tls_configure_socket(ctx, network_get_read_timeout());
#else
    tls_configure_socket(ctx, AGENT_LLM_SOCKET_TIMEOUT_SEC);
#endif

    syslog(LOG_INFO, "[%s] Handshake OK: %s / %s\n", TAG, mbedtls_ssl_get_version(&ctx->ssl),
        mbedtls_ssl_get_ciphersuite(&ctx->ssl));

    return 0;
}

/* ── Connect with retry ──────────────────────────────────────── */

/* The MiMo endpoint (token-plan-cn.xiaomimimo.com, a Xiaomi ALB) intermittently
 * fails fresh handshakes during backend rotation: the server goes silent
 * (recv timeout), serves a cert this build can't parse, or sends an invalid
 * record — all transient, and the same host works moments later. Retry with
 * exponential backoff so a slow rotation isn't exhausted by the fixed 200ms
 * gap; tls_ctx_connect() already iterates every resolved IP per attempt. */
#define TLS_CONNECT_MAX_ATTEMPTS 8
/* Cap the backoff so a long retry run doesn't sleep for minutes between the
 * final attempts. With 8 attempts the gaps are 500/1000/2000/4000/4000/4000/4000ms
 * (~19.5s) plus the per-attempt handshake work — enough to ride out the MiMo
 * ALB's ~50s+ bad windows that the old ~7.5s window (5 attempts) gave up on. */
#define TLS_CONNECT_MAX_BACKOFF_MS 4000

static int tls_ctx_connect_retry(tls_ctx_t* ctx, const char* host,
    const char* port)
{
    int ret = VELA_TLS_ERR_HANDSHAKE;
    /* Backoff in ms: 500, 1000, 2000, 4000, then capped (TLS_CONNECT_MAX_BACKOFF_MS). */
    int backoff_ms = 500;

    for (int attempt = 0; attempt < TLS_CONNECT_MAX_ATTEMPTS; attempt++) {
        ret = tls_ctx_connect(ctx, host, port);

        if (ret == 0) {
            return 0;
        }

        /* Only transient failures (TLS handshake / TCP connect) warrant a
         * retry; anything else (config/entropy) is fatal. */
        if (ret != VELA_TLS_ERR_HANDSHAKE && ret != VELA_TLS_ERR_CONNECT) {
            return ret;
        }

        if (attempt + 1 == TLS_CONNECT_MAX_ATTEMPTS) {
            break; /* last attempt — leave ctx for the caller to free */
        }

        syslog(LOG_WARNING,
            "[%s] connect %s:%s attempt %d/%d failed (%d), retrying in %dms\n",
            TAG, host, port, attempt + 1, TLS_CONNECT_MAX_ATTEMPTS, ret,
            backoff_ms);

        tls_ctx_free(ctx);
        usleep(backoff_ms * 1000);
        if (backoff_ms < TLS_CONNECT_MAX_BACKOFF_MS) {
            backoff_ms *= 2;
        }
    }

    return ret;
}

/* ── HTTP/1.1 framing ────────────────────────────────────────── */

static int tls_write_request(tls_ctx_t* ctx,
    const char* method, const char* host,
    const char* path,
    const vela_header_t* headers,
    const char* body, size_t body_len)
{
    /* Heap-allocate header buffer to reduce stack pressure.
     * This function is called from threads with limited stack
     * (outbound dispatch 16KB) and the TLS context already
     * consumes significant stack space. */
    char* hdr = malloc(4096);
    if (!hdr)
        return VELA_TLS_ERR_OVERFLOW;

    int pos = 0;
    int ret;

#define HDR_APPEND(fmt, ...)                                    \
    pos += snprintf(hdr + pos, 4096 - pos, fmt, ##__VA_ARGS__); \
    if (pos >= 4096) {                                          \
        free(hdr);                                              \
        return VELA_TLS_ERR_OVERFLOW;                           \
    }

    HDR_APPEND("%s %s HTTP/1.1\r\n", method, path);
    HDR_APPEND("Host: %s\r\n", host);
    HDR_APPEND("Connection: keep-alive\r\n");
    HDR_APPEND("User-Agent: agent-vela/1.0\r\n");

    if (body && body_len > 0) {
        HDR_APPEND("Content-Length: %zu\r\n", body_len);
    }

    if (headers) {
        for (const vela_header_t* h = headers; h->name != NULL; h++) {
            HDR_APPEND("%s: %s\r\n", h->name, h->value);
        }
    }

    HDR_APPEND("\r\n");
#undef HDR_APPEND

    /* Write headers */
    int written = 0;
    while (written < pos) {
        ret = mbedtls_ssl_write(&ctx->ssl,
            (const unsigned char*)(hdr + written),
            (size_t)(pos - written));
        if (ret > 0) {
            written += ret;
        } else if (ret == 0) {
            free(hdr);
            return VELA_TLS_ERR_WRITE;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            free(hdr);
            return VELA_TLS_ERR_WRITE;
        }
    }

    free(hdr);

    /* Write body */
    if (body && body_len > 0) {
        size_t bw = 0;
        while (bw < body_len) {
            ret = mbedtls_ssl_write(&ctx->ssl,
                (const unsigned char*)(body + bw),
                body_len - bw);
            if (ret > 0) {
                bw += (size_t)ret;
            } else if (ret == 0) {
                return VELA_TLS_ERR_WRITE;
            } else if (ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
                return VELA_TLS_ERR_WRITE;
            }
        }
    }

    return 0;
}

/**
 * Read full HTTP/1.1 response.
 * Returns HTTP status code; writes body into resp_buf (NUL-terminated).
 * Handles Transfer-Encoding: chunked and Content-Length.
 */
#define TLS_RAW_BUF_SIZE 8192  /* 8KB: enough for HTTP headers + initial body */

/* Number of static raw buffers — at most 2, scaled to pool size.
 * Each buffer is 8KB; pool=1 uses 1 buffer, pool>=2 uses 2. */
#define TLS_RAW_BUF_COUNT (CONN_POOL_SIZE < 2 ? 1 : 2)

static char s_tls_raw_buf[TLS_RAW_BUF_COUNT][TLS_RAW_BUF_SIZE];
static pthread_mutex_t s_tls_raw_lock[TLS_RAW_BUF_COUNT];
static pthread_once_t s_tls_raw_once = PTHREAD_ONCE_INIT;

static void tls_raw_init_once(void)
{
    for (int i = 0; i < TLS_RAW_BUF_COUNT; i++) {
        pthread_mutex_init(&s_tls_raw_lock[i], NULL);
    }
}

static char* tls_raw_acquire(void)
{
    pthread_once(&s_tls_raw_once, tls_raw_init_once);
    for (int i = 0; i < TLS_RAW_BUF_COUNT; i++) {
        if (pthread_mutex_trylock(&s_tls_raw_lock[i]) == 0) {
            return s_tls_raw_buf[i];
        }
    }
    /* All busy — block on first */
    pthread_mutex_lock(&s_tls_raw_lock[0]);
    return s_tls_raw_buf[0];
}

static void tls_raw_release(char* buf)
{
    for (int i = 0; i < TLS_RAW_BUF_COUNT; i++) {
        if (buf == s_tls_raw_buf[i]) {
            pthread_mutex_unlock(&s_tls_raw_lock[i]);
            return;
        }
    }
}

static int tls_read_response(tls_ctx_t* ctx, char* resp_buf, size_t resp_cap,
    size_t* out_body_len, bool* out_keep_alive)
{
    /* Use static buffers to avoid heap fragmentation */
    char* raw = tls_raw_acquire();
    size_t raw_len = 0;
    int eof = 0;
    int ret;

    /* Read until we have the full header (double CRLF) or buffer full */
    while (!eof && raw_len < TLS_RAW_BUF_SIZE - 1) {
        ret = mbedtls_ssl_read(&ctx->ssl,
            (unsigned char*)(raw + raw_len),
            TLS_RAW_BUF_SIZE - 1 - raw_len);
        if (ret > 0) {
            raw_len += (size_t)ret;
            if (memmem(raw, raw_len, "\r\n\r\n", 4))
                break;
        } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            eof = 1;
            break;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            syslog(LOG_ERR, "[%s] ssl_read (header) ret=0x%x\n", TAG, -ret);
            tls_raw_release(raw);
            return VELA_TLS_ERR_READ;
        }
    }
    raw[raw_len] = '\0';

    /* Parse status line */
    int http_status = 0;
    if (sscanf(raw, "HTTP/1.%*d %d", &http_status) != 1) {
        syslog(LOG_ERR, "[%s] Failed to parse HTTP status from: %.80s\n", TAG, raw);
        tls_raw_release(raw);
        return VELA_TLS_ERR_READ;
    }

    /* Find header/body split */
    char* body_start = (char*)memmem(raw, raw_len, "\r\n\r\n", 4);
    if (!body_start) {
        resp_buf[0] = '\0';
        tls_raw_release(raw);
        return http_status;
    }

    /* Determine keep-alive from Connection header (default true for HTTP/1.1) */
    if (out_keep_alive) {
        *out_keep_alive = true;  /* HTTP/1.1 default */
        char* conn_hdr = strcasestr(raw, "Connection:");
        if (conn_hdr && conn_hdr < body_start) {
            *out_keep_alive = (strcasestr(conn_hdr, "keep-alive") != NULL);
        }
    }
    body_start += 4; /* skip double CRLF */

    /* Extract content-length if present */
    long content_length = -1;
    {
        char* cl_hdr = strcasestr(raw, "Content-Length:");
        if (cl_hdr && cl_hdr < body_start) {
            cl_hdr += strlen("Content-Length:");
            content_length = strtol(cl_hdr, NULL, 10);
            if (content_length < 0 || content_length > 10 * 1024 * 1024) {
                content_length = -1; /* reject absurd values */
            }
        }
    }
    int chunked = 0;
    {
        char* te_hdr = strcasestr(raw, "Transfer-Encoding:");
        if (te_hdr && te_hdr < body_start) {
            chunked = (strcasestr(te_hdr, "chunked") != NULL);
        }
    }

    /* Initial fragment already in raw buffer */
    size_t initial = (size_t)(raw + raw_len - body_start);
    size_t resp_pos = 0;

    /* Copy initial fragment */
    size_t copy = initial < resp_cap - 1 ? initial : resp_cap - 1;
    memcpy(resp_buf, body_start, copy);
    resp_pos = copy;

    /* Keep reading body */
    if (!eof) {
        while (resp_pos < resp_cap - 1) {
            if (content_length >= 0 && (long)resp_pos >= content_length)
                break;
            /* A chunked body is fully received once its final chunk ("0") has
             * arrived. Stop then rather than blocking on a keep-alive peer
             * that leaves the connection open (MiMo TTS answers ~20s of stall
             * per sentence waiting for the server's idle close). */
            if (chunked && chunked_body_complete(resp_buf, resp_pos))
                break;
            ret = mbedtls_ssl_read(&ctx->ssl,
                (unsigned char*)(resp_buf + resp_pos),
                resp_cap - 1 - resp_pos);
            if (ret > 0) {
                resp_pos += (size_t)ret;
            } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                break;
            } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
                break;
            }
        }
    }

    resp_buf[resp_pos] = '\0';
    tls_raw_release(raw);

    /* Chunked decode */
    if (chunked) {
        resp_pos = decode_chunked(resp_buf, resp_pos);
        resp_buf[resp_pos] = '\0';
    }

    if (out_body_len)
        *out_body_len = resp_pos;

    return http_status;
}

/* ── Streaming response read ─────────────────────────────────── */

/* Incremental chunked-transfer decoder.  Chunked framing is
 *   <hex-size>\r\n<data>\r\n ... 0\r\n\r\n
 * and the chunk headers/data may be split arbitrarily across TLS reads,
 * so we carry incomplete bytes between calls.  Fully-received chunk
 * payloads are emitted to cb(). */
typedef struct {
    char  buf[8192];
    size_t len;
} chunk_decoder_t;

/* Feed `len` raw (still-encoded) body bytes into the decoder.
 * Returns 1 when the final (zero-size) chunk has been consumed, 0 while
 * more data is expected, -1 on malformed framing. */
static int chunk_decoder_feed(chunk_decoder_t *cd, const char *data,
    size_t len, vela_stream_cb_t cb, void *ctx)
{
    while (len > 0) {
        size_t room = sizeof(cd->buf) - cd->len;
        size_t n = len < room ? len : room;
        memcpy(cd->buf + cd->len, data, n);
        cd->len += n;
        data += n;
        len -= n;
        if (cd->len == sizeof(cd->buf)) {
            syslog(LOG_ERR, "[%s] chunk decoder overflow\n", TAG);
            return -1;
        }
    }

    for (;;) {
        char *crlf = (char *)memmem(cd->buf, cd->len, "\r\n", 2);
        if (!crlf)
            break; /* incomplete size line */

        char *endptr = NULL;
        long chunk_sz = strtol(cd->buf, &endptr, 16);
        while (endptr < crlf && *endptr == ' ')
            endptr++;
        if (endptr == cd->buf || endptr != crlf || chunk_sz < 0) {
            syslog(LOG_ERR, "[%s] malformed chunk header\n", TAG);
            return -1;
        }

        size_t hdr_len = (size_t)(crlf - cd->buf) + 2; /* size line + CRLF */

        if (chunk_sz == 0) {
            cd->len = 0; /* final chunk — done */
            return 1;
        }

        size_t need = hdr_len + (size_t)chunk_sz + 2; /* payload + CRLF */
        if (cd->len < need)
            break; /* not all of this chunk has arrived yet */

        if (cb(cd->buf + hdr_len, (size_t)chunk_sz, ctx) != 0)
            return 1; /* caller aborted */

        size_t consumed = hdr_len + (size_t)chunk_sz + 2;
        memmove(cd->buf, cd->buf + consumed, cd->len - consumed);
        cd->len -= consumed;
    }

    return 0;
}

/* Read a response, delivering decoded body bytes to cb() as they arrive.
 * Mirrors tls_read_response() but does not buffer the whole body. */
static int tls_read_response_stream(tls_ctx_t *ctx, vela_stream_cb_t cb,
    void *uctx)
{
    char *raw = tls_raw_acquire();
    size_t raw_len = 0;
    int ret;
    int result = VELA_TLS_ERR_READ;
    chunk_decoder_t *cd = NULL;
    char *tmp = NULL;

    /* Read until full header (double CRLF). */
    while (raw_len < TLS_RAW_BUF_SIZE - 1) {
        ret = mbedtls_ssl_read(&ctx->ssl,
            (unsigned char *)(raw + raw_len), TLS_RAW_BUF_SIZE - 1 - raw_len);
        if (ret > 0) {
            raw_len += (size_t)ret;
            if (memmem(raw, raw_len, "\r\n\r\n", 4))
                break;
        } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            break;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            syslog(LOG_ERR, "[%s] ssl_read (header) ret=0x%x\n", TAG, -ret);
            goto cleanup;
        }
    }
    raw[raw_len] = '\0';

    int http_status = 0;
    if (sscanf(raw, "HTTP/1.%*d %d", &http_status) != 1) {
        syslog(LOG_ERR, "[%s] Failed to parse HTTP status\n", TAG);
        goto cleanup;
    }
    result = http_status;

    char *body_start = (char *)memmem(raw, raw_len, "\r\n\r\n", 4);
    if (!body_start) {
        goto cleanup; /* no body */
    }
    body_start += 4;

    long content_length = -1;
    {
        char *cl_hdr = strcasestr(raw, "Content-Length:");
        if (cl_hdr && cl_hdr < body_start) {
            content_length = strtol(cl_hdr + strlen("Content-Length:"), NULL, 10);
            if (content_length < 0 || content_length > 10 * 1024 * 1024)
                content_length = -1;
        }
    }
    int chunked = 0;
    {
        char *te_hdr = strcasestr(raw, "Transfer-Encoding:");
        if (te_hdr && te_hdr < body_start)
            chunked = (strcasestr(te_hdr, "chunked") != NULL);
    }

    /* Initial fragment (bytes already read past the header). */
    size_t initial = (size_t)(raw + raw_len - body_start);
    long emitted = (long)initial;

    /* Heap-allocate the chunk decoder + read buffer: the caller's task stack
     * (agent loop) is only 32KB and already carries a tls_ctx_t. */
    cd = calloc(1, sizeof(*cd));
    tmp = malloc(4096);
    if (!cd || !tmp) {
        goto cleanup;
    }

    if (chunked) {
        int r = chunk_decoder_feed(cd, body_start, initial, cb, uctx);
        if (r < 0) {
            result = VELA_TLS_ERR_READ;
            goto cleanup;
        }
        if (r == 1) {
            goto cleanup; /* final chunk already received */
        }
    } else if (initial > 0) {
        if (cb(body_start, initial, uctx) != 0) {
            goto cleanup;
        }
        if (content_length >= 0 && emitted >= content_length) {
            goto cleanup;
        }
    }
    tls_raw_release(raw);
    raw = NULL;

    /* Keep reading the body. */
    for (;;) {
        ret = mbedtls_ssl_read(&ctx->ssl, (unsigned char *)tmp, 4096);
        if (ret > 0) {
            if (chunked) {
                int r = chunk_decoder_feed(cd, tmp, (size_t)ret, cb, uctx);
                if (r < 0) {
                    result = VELA_TLS_ERR_READ;
                    break;
                }
                if (r == 1)
                    break; /* final chunk — stop reading, don't wait for EOF */
            } else {
                if (cb(tmp, (size_t)ret, uctx) != 0)
                    break;
                emitted += ret;
                if (content_length >= 0 && emitted >= content_length)
                    break;
            }
        } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            break;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            break;
        }
    }

cleanup:
    free(cd);
    free(tmp);
    if (raw)
        tls_raw_release(raw);
    return result;
}

/* ── Public API ──────────────────────────────────────────────── */

int vela_https_request(
    const char* host,
    const char* port,
    const char* method,
    const char* path,
    const vela_header_t* headers,
    const char* body,
    size_t body_len,
    char* resp_buf,
    size_t resp_cap,
    size_t* out_body_len)
{
    int ret;

    /* Try to get a pooled connection first */
    conn_slot_t* slot = pool_acquire(host, port);

    if (slot && slot->valid) {
        /* Drain any leftover data from previous response before reuse.
         * Without this, a partially-read response body (e.g. truncated
         * chunked data) would be misinterpreted as the next HTTP status. */
        if (slot->ctx.net.fd >= 0) {
            unsigned char drain[512];
            int dr;
            /* Temporarily set a very short socket timeout to drain without blocking */
            struct timeval tv_drain = { .tv_sec = 0, .tv_usec = 10000 }; /* 10ms */
            struct timeval tv_orig = { .tv_sec = AGENT_LLM_SOCKET_TIMEOUT_SEC,
                .tv_usec = 0 };
            setsockopt(slot->ctx.net.fd, SOL_SOCKET, SO_RCVTIMEO,
                &tv_drain, sizeof(tv_drain));
            while ((dr = mbedtls_ssl_read(&slot->ctx.ssl, drain, sizeof(drain))) > 0)
                ; /* discard leftover bytes */
            /* Restore original timeout */
            setsockopt(slot->ctx.net.fd, SOL_SOCKET, SO_RCVTIMEO,
                &tv_orig, sizeof(tv_orig));
            /* If peer closed the connection, reconnect */
            if (dr == 0 || dr == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                goto pool_reconnect;
            }
        } else {
            goto pool_reconnect;
        }

        /* Reuse existing connection — skip TLS handshake */
        syslog(LOG_DEBUG, "[%s] Reusing pooled connection to %s:%s\n", TAG, host, port);
        ret = tls_write_request(&slot->ctx, method, host, path, headers, body, body_len);
        if (ret == 0) {
            bool keep = false;
            ret = tls_read_response(&slot->ctx, resp_buf, resp_cap, out_body_len, &keep);
            if (ret > 0) {
                pool_release(slot, host, port, keep);
                return ret;
            }
        }
        /* Connection went stale — fall through to reconnect */
    pool_reconnect:
        syslog(LOG_INFO, "[%s] Pooled connection stale, reconnecting\n", TAG);
        tls_ctx_free(&slot->ctx);
        slot->valid = false;
    }

    /* New connection */
    if (!slot) {
        /* Pool full — use a temporary ephemeral context */
        tls_ctx_t tmp_ctx;
        if ((ret = tls_ctx_connect_retry(&tmp_ctx, host, port)) != 0) {
            tls_ctx_free(&tmp_ctx);
            return ret;
        }
        if ((ret = tls_write_request(&tmp_ctx, method, host, path,
                 headers, body, body_len))
            != 0) {
            syslog(LOG_ERR, "[%s] Write request failed: %d\n", TAG, ret);
            tls_ctx_free(&tmp_ctx);
            return ret;
        }
        ret = tls_read_response(&tmp_ctx, resp_buf, resp_cap, out_body_len, NULL);
        tls_ctx_free(&tmp_ctx);
        return ret;
    }

    /* Connect into the slot */
    if ((ret = tls_ctx_connect_retry(&slot->ctx, host, port)) != 0) {
        pool_release(slot, host, port, false);
        return ret;
    }

    if ((ret = tls_write_request(&slot->ctx, method, host, path,
             headers, body, body_len))
        != 0) {
        syslog(LOG_ERR, "[%s] Write request failed: %d\n", TAG, ret);
        pool_release(slot, host, port, false);
        return ret;
    }

    bool keep = false;
    ret = tls_read_response(&slot->ctx, resp_buf, resp_cap, out_body_len, &keep);
    pool_release(slot, host, port, ret > 0 && keep);
    return ret;
}

int vela_https_get(const char* host, const char* port, const char* path,
    char* resp_buf, size_t resp_cap)
{
    return vela_https_request(host, port, "GET", path, NULL, NULL, 0,
        resp_buf, resp_cap, NULL);
}

int vela_https_post_json(const char* host, const char* port, const char* path,
    const vela_header_t* extra_headers,
    const char* json_body,
    char* resp_buf, size_t resp_cap)
{
    /* Build a merged header list: Content-Type first, then caller extras */
    const int MAX_HDRS = 32;
    vela_header_t merged[MAX_HDRS];
    int n = 0;

    merged[n++] = (vela_header_t) { "Content-Type", "application/json" };

    if (extra_headers) {
        for (const vela_header_t* h = extra_headers; h->name && n < MAX_HDRS - 1; h++) {
            merged[n++] = *h;
        }
    }
    merged[n] = (vela_header_t) { NULL, NULL };

    size_t body_len = json_body ? strlen(json_body) : 0;
    return vela_https_request(host, port, "POST", path, merged,
        json_body, body_len, resp_buf, resp_cap, NULL);
}

int vela_https_post_json_stream(const char* host, const char* port,
    const char* path, const vela_header_t* extra_headers,
    const char* json_body, vela_stream_cb_t cb, void* ctx)
{
    const int MAX_HDRS = 32;
    vela_header_t merged[MAX_HDRS];
    int n = 0;

    merged[n++] = (vela_header_t) { "Content-Type", "application/json" };

    if (extra_headers) {
        for (const vela_header_t* h = extra_headers; h->name && n < MAX_HDRS - 1; h++) {
            merged[n++] = *h;
        }
    }
    merged[n] = (vela_header_t) { NULL, NULL };

    size_t body_len = json_body ? strlen(json_body) : 0;
    tls_ctx_t tls;
    int ret;

    /* Always use a fresh connection: a streamed response stays open for the
     * whole generation and is not compatible with pooled keep-alive reuse. */
    if ((ret = tls_ctx_connect_retry(&tls, host, port)) != 0) {
        tls_ctx_free(&tls);
        return ret;
    }

    if ((ret = tls_write_request(&tls, "POST", host, path, merged,
             json_body, body_len)) != 0) {
        syslog(LOG_ERR, "[%s] Write request failed: %d\n", TAG, ret);
        tls_ctx_free(&tls);
        return ret;
    }

    ret = tls_read_response_stream(&tls, cb, ctx);
    tls_ctx_free(&tls);
    return ret;
}

int vela_https_head_date(const char* host, const char* port, const char* path,
    char* date_out, size_t date_cap)
{
    tls_ctx_t ctx;
    int ret;

    if ((ret = tls_ctx_connect(&ctx, host, port)) != 0) {
        tls_ctx_free(&ctx);
        return ret;
    }

    if ((ret = tls_write_request(&ctx, "HEAD", host, path, NULL, NULL, 0)) != 0) {
        tls_ctx_free(&ctx);
        return ret;
    }

    /* Read raw response until end of headers (\r\n\r\n) */
    char hdr_buf[2048];
    int total = 0;
    while (total < (int)sizeof(hdr_buf) - 1) {
        ret = mbedtls_ssl_read(&ctx.ssl,
            (unsigned char*)hdr_buf + total,
            sizeof(hdr_buf) - 1 - total);
        if (ret == MBEDTLS_ERR_SSL_WANT_READ)
            continue;
        if (ret <= 0)
            break;
        total += ret;
        hdr_buf[total] = '\0';
        if (strstr(hdr_buf, "\r\n\r\n"))
            break;
    }
    tls_ctx_free(&ctx);

    if (total <= 0)
        return VELA_TLS_ERR_READ;

    /* Find Date: header (case-insensitive prefix search) */
    char* p = strcasestr(hdr_buf, "\r\nDate: ");
    if (!p)
        return VELA_TLS_ERR_READ;
    p += 8; /* skip \r\nDate:  */
    char* eol = strstr(p, "\r\n");
    if (!eol)
        return VELA_TLS_ERR_READ;

    size_t dlen = (size_t)(eol - p);
    if (dlen >= date_cap)
        dlen = date_cap - 1;
    memcpy(date_out, p, dlen);
    date_out[dlen] = '\0';
    return 0;
}

/* ── Plain HTTP (no TLS) POST ─────────────────────────────── */

int vela_http_post_json(const char* host, const char* port, const char* path,
    const vela_header_t* extra_headers,
    const char* json_body,
    char* resp_buf, size_t resp_cap)
{
    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int gai = getaddrinfo(host, port, &hints, &res);
    if (gai != 0 || !res) {
        syslog(LOG_INFO, "http: getaddrinfo %s:%s failed: %d", host, port, gai);
        return VELA_TLS_ERR_CONNECT;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        syslog(LOG_INFO, "http: socket() failed: %d", errno);
        return VELA_TLS_ERR_CONNECT;
    }

    if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        syslog(LOG_INFO, "http: connect %s:%s failed: %d", host, port, errno);
        close(fd);
        freeaddrinfo(res);
        return VELA_TLS_ERR_CONNECT;
    }
    freeaddrinfo(res);

    /* Set read timeout */
    struct timeval tv = { .tv_sec = AGENT_LLM_SOCKET_TIMEOUT_SEC,
        .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* Build HTTP request */
    size_t body_len = json_body ? strlen(json_body) : 0;
    char hdr[4096];
    int pos = 0;

#define HTTP_APPEND(fmt, ...)                                               \
    pos += snprintf(hdr + pos, (int)sizeof(hdr) - pos, fmt, ##__VA_ARGS__); \
    if (pos >= (int)sizeof(hdr)) {                                          \
        close(fd);                                                          \
        return VELA_TLS_ERR_OVERFLOW;                                       \
    }

    HTTP_APPEND("POST %s HTTP/1.1\r\n", path);
    HTTP_APPEND("Host: %s\r\n", host);
    HTTP_APPEND("Content-Type: application/json\r\n");
    HTTP_APPEND("Connection: close\r\n");
    HTTP_APPEND("User-Agent: agent/1.0\r\n");
    if (body_len > 0) {
        HTTP_APPEND("Content-Length: %zu\r\n", body_len);
    }
    if (extra_headers) {
        for (const vela_header_t* h = extra_headers; h->name; h++) {
            HTTP_APPEND("%s: %s\r\n", h->name, h->value);
        }
    }
    HTTP_APPEND("\r\n");
#undef HTTP_APPEND

    /* Send header + body */
    if (write(fd, hdr, (size_t)pos) != pos) {
        close(fd);
        return VELA_TLS_ERR_WRITE;
    }
    if (json_body && body_len > 0) {
        if (write(fd, json_body, body_len) != (ssize_t)body_len) {
            close(fd);
            return VELA_TLS_ERR_WRITE;
        }
    }

    /* Read response */
    char* raw = (char*)malloc(TLS_RAW_BUF_SIZE);
    if (!raw) {
        close(fd);
        return VELA_TLS_ERR_READ;
    }
    size_t raw_len = 0;
    int eof = 0;

    /* Read until we have the full header */
    while (!eof && raw_len < TLS_RAW_BUF_SIZE - 1) {
        ssize_t n = read(fd, raw + raw_len, TLS_RAW_BUF_SIZE - 1 - raw_len);
        if (n > 0) {
            raw_len += (size_t)n;
            raw[raw_len] = '\0';
            if (memmem(raw, raw_len, "\r\n\r\n", 4))
                break;
        } else {
            eof = 1;
        }
    }
    raw[raw_len] = '\0';

    /* Parse status */
    int http_status = 0;
    if (sscanf(raw, "HTTP/1.%*d %d", &http_status) != 1) {
        syslog(LOG_INFO, "http: bad status: %.80s", raw);
        free(raw);
        close(fd);
        return VELA_TLS_ERR_READ;
    }

    /* Find body */
    char* body_start = (char*)memmem(raw, raw_len, "\r\n\r\n", 4);
    if (!body_start) {
        resp_buf[0] = '\0';
        free(raw);
        close(fd);
        return http_status;
    }
    body_start += 4;

    /* Check content-length / chunked */
    long content_length = -1;
    {
        char* cl = strcasestr(raw, "Content-Length:");
        if (cl && cl < body_start) {
            content_length = strtol(cl + strlen("Content-Length:"), NULL, 10);
            if (content_length < 0 || content_length > 10 * 1024 * 1024) {
                content_length = -1;
            }
        }
    }
    int chunked = 0;
    {
        char* te = strcasestr(raw, "Transfer-Encoding:");
        if (te && te < body_start) {
            chunked = (strcasestr(te, "chunked") != NULL);
        }
    }

    /* Copy initial fragment */
    size_t initial = (size_t)(raw + raw_len - body_start);
    size_t resp_pos = 0;
    size_t copy = initial < resp_cap - 1 ? initial : resp_cap - 1;
    memcpy(resp_buf, body_start, copy);
    resp_pos = copy;

    /* Keep reading body */
    if (!eof) {
        while (resp_pos < resp_cap - 1) {
            if (content_length >= 0 && (long)resp_pos >= content_length)
                break;
            ssize_t n = read(fd, resp_buf + resp_pos, resp_cap - 1 - resp_pos);
            if (n <= 0)
                break;
            resp_pos += (size_t)n;
        }
    }
    resp_buf[resp_pos] = '\0';
    free(raw);
    close(fd);

    /* Chunked decode */
    if (chunked) {
        resp_pos = decode_chunked(resp_buf, resp_pos);
        resp_buf[resp_pos] = '\0';
    }

    return http_status;
}
