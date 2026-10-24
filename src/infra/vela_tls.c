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
#include "infra/http_body.h"
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
#include <arpa/inet.h>
#if defined(__NuttX__) && defined(CONFIG_NETDB_DNSCLIENT)
#  include <nuttx/net/dns.h>
#endif
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
#include <poll.h>
#include <time.h>

#ifdef CONFIG_AI_AGENT_TLS_FORENSICS
#include <sys/stat.h>
#include "mbedtls/md.h"
#include "mbedtls/x509_crt.h"
#endif
#ifdef CONFIG_AI_AGENT_TLS_POOL
#include "mbedtls/platform.h"
#endif

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

/* ── TLS context ─────────────────────────────────────────────── */

typedef struct {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config cfg;
    mbedtls_net_context net;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool abort_on_close;
    unsigned send_pressure;
#ifdef CONFIG_AI_AGENT_TLS_FORENSICS
    /* Inner BIO pair captured by the forensic wrappers below. */
    int (*fr_send)(void *, const unsigned char *, size_t);
    int (*fr_recv)(void *, unsigned char *, size_t);
    void *fr_bio;
    bool fr_owned;   /* this connection owns the global capture slot */
#endif
} tls_ctx_t;

/* NuttX may report temporary packet-buffer pressure as ENOMEM, including
 * during handshake. Translate it at the BIO boundary so mbedTLS retains
 * the pending TLS record and the controlled poll loop applies its budget. */
static int tls_controlled_send(void *arg, const unsigned char *buf, size_t len)
{
    tls_ctx_t *ctx = arg;
    int ret = mbedtls_net_send(&ctx->net, buf, len);
    int error = errno;
    if (ret == MBEDTLS_ERR_NET_SEND_FAILED && error == ENOMEM) {
        if (ctx->send_pressure++ == 0)
            syslog(LOG_INFO, "[%s] controlled send waiting for packet buffers\n", TAG);
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    }
    return ret;
}

static int tls_controlled_recv(void *arg, unsigned char *buf, size_t len)
{
    tls_ctx_t *ctx = arg;
    return mbedtls_net_recv(&ctx->net, buf, len);
}

static void tls_ctx_free(tls_ctx_t* ctx)
{
    if (ctx->abort_on_close) {
#ifdef CONFIG_NET_SOLINGER
        if (ctx->net.fd >= 0) {
            struct linger abort = {1, 0};
            setsockopt(ctx->net.fd, SOL_SOCKET, SO_LINGER, &abort, sizeof(abort));
        }
#endif
    } else {
        mbedtls_ssl_close_notify(&ctx->ssl);
    }
    /* close_notify already finishes TLS.  A separate shutdown followed by
     * close schedules overlapping TCP callbacks on NuttX, and controlled
     * callers used to shut down twice.  Close the socket exactly once. */
    mbedtls_net_close(&ctx->net);
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

static void tls_voice_session_clear(void);

#ifdef CONFIG_AI_AGENT_VOICE_CONN_SLOT

/* ── voice-dedicated keep-alive slot (P2 phase 1) ──────────────────
 *
 * ASR and TTS run on the voice round thread against the same endpoint,
 * so one connection slot reused sequentially removes the per-request
 * DNS+TCP+TLS handshake (1-2.5 s on this board).  The slot is separate
 * from s_pool: the buffered pool serves background callers and mixing
 * them would let a background request evict the foreground connection
 * mid-round.  LLM requests (agent loop thread) keep the plain entry in
 * this phase — cross-thread handoff is a separate change.
 *
 * Publishing rules: a connection may be kept only when the response
 * framing completed (Content-Length satisfied or the terminating zero
 * chunk consumed).  A sink that reported completion early — the TTS
 * sink returns 1 on [DONE] — leaves the terminator unread and needs a
 * bounded drain first.  Anything else (error, cancellation, EOF-
 * delimited body, failed drain) destroys the connection. */

static conn_slot_t s_voice_slot;
static pthread_mutex_t s_voice_slot_lock = PTHREAD_MUTEX_INITIALIZER;

/* Returns the slot marked in_use, or NULL when it is busy (e.g. a
 * preconnect is still running) — the caller then goes ephemeral.  A
 * valid slot for a different target is worthless and freed. */
static conn_slot_t* voice_slot_try_acquire(const char* host, const char* port)
{
    pthread_mutex_lock(&s_voice_slot_lock);

    if (s_voice_slot.in_use) {
        pthread_mutex_unlock(&s_voice_slot_lock);
        return NULL;
    }

    if (s_voice_slot.valid
        && (strcmp(s_voice_slot.host, host) != 0
            || strcmp(s_voice_slot.port, port) != 0)) {
        tls_ctx_free(&s_voice_slot.ctx);
        s_voice_slot.valid = false;
        s_voice_slot.host[0] = '\0';
    }

    s_voice_slot.in_use = true;
    pthread_mutex_unlock(&s_voice_slot_lock);
    return &s_voice_slot;
}

/* The caller has already freed the ctx when keep is false. */
static void voice_slot_release(const char* host, const char* port, bool keep)
{
    pthread_mutex_lock(&s_voice_slot_lock);
    if (keep) {
        strncpy(s_voice_slot.host, host, sizeof(s_voice_slot.host) - 1);
        strncpy(s_voice_slot.port, port, sizeof(s_voice_slot.port) - 1);
        s_voice_slot.valid = true;
    } else {
        s_voice_slot.valid = false;
        s_voice_slot.host[0] = '\0';
    }
    s_voice_slot.in_use = false;
    pthread_mutex_unlock(&s_voice_slot_lock);
}

/* One short read to decide whether a kept connection is still alive:
 * WANT_READ/WANT_WRITE (the 10 ms socket timeout) means quiet and
 * alive; 0 or PEER_CLOSE_NOTIFY means the server closed it; payload
 * bytes mean the previous round's bookkeeping was wrong — destroy
 * rather than guess. */
static bool voice_slot_alive(tls_ctx_t* ctx)
{
    unsigned char probe[64];
    struct timeval tv = { .tv_sec = 0, .tv_usec = 10000 };
    int n;

    if (ctx->net.fd < 0) {
        return false;
    }

    setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    n = mbedtls_ssl_read(&ctx->ssl, probe, sizeof(probe));

    return n == MBEDTLS_ERR_SSL_WANT_READ
        || n == MBEDTLS_ERR_SSL_WANT_WRITE
        || n == MBEDTLS_ERR_SSL_TIMEOUT;
}

/* Pure keep-decision so the unit tests can drive the truth table
 * without sockets. Socket silence and a sink end marker are not proof
 * that the HTTP response ended. */
static bool voice_slot_keep(int ret, bool framing_complete, bool response_close)
{
    return ret == 0 && framing_complete && !response_close;
}

#endif /* CONFIG_AI_AGENT_VOICE_CONN_SLOT */

void vela_tls_pool_cleanup(void)
{
    tls_voice_session_clear();
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

#ifdef CONFIG_AI_AGENT_VOICE_CONN_SLOT
    pthread_mutex_lock(&s_voice_slot_lock);
    if (s_voice_slot.in_use) {
        syslog(LOG_WARNING,
            "[vela_tls] Voice slot still in use at shutdown, skipping\n");
    } else if (s_voice_slot.valid) {
        tls_ctx_free(&s_voice_slot.ctx);
        s_voice_slot.valid = false;
        s_voice_slot.host[0] = '\0';
    }
    pthread_mutex_unlock(&s_voice_slot_lock);
#endif
}

/* Timing is optional so the other callers (Feishu, vision, web search) keep
 * their current behaviour.  The marks are taken back to back, so
 * dns_tcp + tls_setup + tls add up to the whole connect. */

/* Wait in short poll slices on controlled requests.  The legacy chat path
 * keeps its existing blocking transport when ctl is NULL. */
static int tls_wait_socket(int fd, short events, const vela_req_ctl_t *ctl)
{
    struct pollfd pfd = { .fd = fd, .events = events };
    uint32_t started = agent_mono_ms();

    for (;;) {
        uint32_t now = agent_mono_ms();
        uint32_t slice;
        int ret;

        if (vela_req_cancelled(ctl)) {
            return VELA_TLS_ERR_CANCELLED;
        }
        if (vela_req_budget_ms(ctl, now) == 0
            || agent_elapsed_ms(started, now) >= 60000) {
            return VELA_TLS_ERR_TIMEOUT;
        }
        slice = vela_req_io_timeout_ms(ctl, now, 100);
        ret = poll(&pfd, 1, (int)slice);
        if (ret > 0) {
            return (pfd.revents & POLLNVAL) ? VELA_TLS_ERR_CONNECT : 0;
        }
        if (ret < 0 && errno != EINTR) {
            return VELA_TLS_ERR_CONNECT;
        }
    }
}

#if defined(__NuttX__) && defined(CONFIG_NETDB_DNSCLIENT)
#if defined(CONFIG_VG_HMI)
static struct sockaddr_storage voice_dns_addrs[4];
static int voice_dns_count;
static uint32_t voice_dns_saved;
static pthread_mutex_t voice_dns_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

static int tls_dns_budget(void *arg)
{
    const vela_req_ctl_t *ctl = arg;
    if (vela_req_cancelled(ctl)) return -ECANCELED;
    int32_t remaining = vela_req_budget_ms(ctl, agent_mono_ms());
    return remaining < 0 ? INT32_MAX : remaining;
}

static int tls_resolve_controlled(const char *host, const char *port,
    const struct addrinfo *hints, struct addrinfo **addresses,
    const vela_req_ctl_t *ctl)
{
    struct addrinfo numeric = *hints;
    struct sockaddr_storage resolved[4];
    struct dns_query_control_s control = {0};
    struct addrinfo **tail = addresses;
    int n = 4;
    int ret;

    numeric.ai_flags |= AI_NUMERICHOST | AI_NUMERICSERV;
    ret = getaddrinfo(host, port, &numeric, addresses);
    if (ret == 0) return 0;

#if defined(CONFIG_VG_HMI)
    bool is_target = (host != NULL && strcmp(host, "token-plan-cn.xiaomimimo.com") == 0);
    uint32_t now = agent_mono_ms();
    if (is_target) {
        pthread_mutex_lock(&voice_dns_lock);
        if (voice_dns_count > 0 && (uint32_t)(now - voice_dns_saved) < 300000) {
            n = voice_dns_count;
            memcpy(resolved, voice_dns_addrs, sizeof(struct sockaddr_storage) * (size_t)n);
            pthread_mutex_unlock(&voice_dns_lock);
            goto convert;
        }
        pthread_mutex_unlock(&voice_dns_lock);
    }
#endif

    control.budget_ms = tls_dns_budget;
    control.arg = (void *)ctl;
    ret = dns_query_controlled(host, resolved, &n, &control);
    if (ret < 0) return ret;

#if defined(CONFIG_VG_HMI)
    if (is_target && n > 0) {
        pthread_mutex_lock(&voice_dns_lock);
        voice_dns_count = n > 4 ? 4 : n;
        memcpy(voice_dns_addrs, resolved, sizeof(struct sockaddr_storage) * (size_t)voice_dns_count);
        voice_dns_saved = now;
        pthread_mutex_unlock(&voice_dns_lock);
    }
convert:
#endif
    for (int i = 0; i < n; i++) {
        char ip[INET6_ADDRSTRLEN];
        const void *address;
        struct addrinfo *entry = NULL;
        if (resolved[i].ss_family == AF_INET) {
            address = &((struct sockaddr_in *)&resolved[i])->sin_addr;
        } else if (resolved[i].ss_family == AF_INET6) {
            address = &((struct sockaddr_in6 *)&resolved[i])->sin6_addr;
        } else continue;
        if (!inet_ntop(resolved[i].ss_family, address, ip, sizeof(ip))) continue;
        if (getaddrinfo(ip, port, &numeric, &entry) == 0) {
            *tail = entry;
            while (*tail != NULL) tail = &(*tail)->ai_next;
        }
    }
    return *addresses != NULL ? 0 : -EHOSTUNREACH;
}
#endif

static int tls_connect_controlled(tls_ctx_t *ctx, const char *host,
    const char *port, const vela_req_ctl_t *ctl)
{
    struct addrinfo hints = {0};
    struct addrinfo *addresses = NULL;
    struct addrinfo *a;
    int result = VELA_TLS_ERR_CONNECT;

    /* On NuttX, DNS polls the same request budget without a helper thread.
     * Numeric address/service conversion cannot initiate another DNS query.
     * Other platforms retain their platform resolver. */
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
#if defined(__NuttX__) && defined(CONFIG_NETDB_DNSCLIENT)
    int ret = tls_resolve_controlled(host, port, &hints, &addresses, ctl);
#else
    int ret = getaddrinfo(host, port, &hints, &addresses);
#endif
    if (vela_req_cancelled(ctl)) {
        result = VELA_TLS_ERR_CANCELLED;
        goto out;
    }
    if (vela_req_budget_ms(ctl, agent_mono_ms()) == 0) {
        result = VELA_TLS_ERR_TIMEOUT;
        goto out;
    }
    if (ret != 0) {
        goto out;
    }

    for (a = addresses; a != NULL; a = a->ai_next) {
        int flags;
        int error = 0;
        socklen_t error_len = sizeof(error);

        /* NSH commands and daemons may spawn while this request is live.
         * Inheriting its descriptor would outlive the transport owner. */
        ctx->net.fd = socket(a->ai_family, a->ai_socktype | SOCK_CLOEXEC,
                             a->ai_protocol);
        if (ctx->net.fd < 0) {
            syslog(LOG_ERR, "[%s] socket allocation failed errno=%d\n", TAG, errno);
            continue;
        }
        flags = fcntl(ctx->net.fd, F_GETFL, 0);
        if (flags < 0 || fcntl(ctx->net.fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            goto next;
        }
        ret = connect(ctx->net.fd, a->ai_addr, a->ai_addrlen);
        if (ret == 0) {
            result = 0;
            break;
        }
        if (errno != EINPROGRESS && errno != EWOULDBLOCK && errno != EALREADY) {
            goto next;
        }
        ret = tls_wait_socket(ctx->net.fd, POLLOUT, ctl);
        if (ret != 0) {
            result = ret;
            break;
        }
        if (getsockopt(ctx->net.fd, SOL_SOCKET, SO_ERROR, &error, &error_len) == 0
            && error == 0) {
            result = 0;
            break;
        }
next:
        mbedtls_net_close(&ctx->net);
    }
out:
    if (addresses != NULL) {
        freeaddrinfo(addresses);
    }
#if defined(__NuttX__) && defined(CONFIG_NETDB_DNSCLIENT) && defined(CONFIG_VG_HMI)
    if (result != 0 && host != NULL && strcmp(host, "token-plan-cn.xiaomimimo.com") == 0) {
        pthread_mutex_lock(&voice_dns_lock);
        voice_dns_count = 0;
        pthread_mutex_unlock(&voice_dns_lock);
    }
#endif
    return result;
}

/* 仅对 HMI 已知服务的受控连接缓存 TLS 1.2 会话，恢复资格一分钟后失效。
 * 不保存到文件，不缓存 HTTP 凭据；服务端可拒绝恢复并执行完整握手。 */
#if defined(CONFIG_VG_HMI) && defined(CONFIG_VG_VOICE_TLS_CACHE) && defined(MBEDTLS_SSL_CLI_C)
static pthread_mutex_t voice_session_lock = PTHREAD_MUTEX_INITIALIZER;
static mbedtls_ssl_session voice_session;
static pthread_once_t voice_session_once = PTHREAD_ONCE_INIT;
static bool voice_session_valid;
static uint32_t voice_session_saved;

static bool tls_voice_session_target(const char *host, const char *port,
                                     const vela_req_ctl_t *ctl)
{
    return ctl != NULL && host != NULL && port != NULL &&
        strcmp(host, "token-plan-cn.xiaomimimo.com") == 0 && strcmp(port, "443") == 0;
}

static void tls_voice_session_init_once(void)
{
    mbedtls_ssl_session_init(&voice_session);
}

static void tls_voice_session_clear(void)
{
    pthread_once(&voice_session_once, tls_voice_session_init_once);
    pthread_mutex_lock(&voice_session_lock);
    mbedtls_ssl_session_free(&voice_session);
    mbedtls_ssl_session_init(&voice_session);
    voice_session_valid = false;
    pthread_mutex_unlock(&voice_session_lock);
}

static bool tls_voice_session_offer(mbedtls_ssl_context *ssl, const char *host,
                                    const char *port, const vela_req_ctl_t *ctl,
                                    uint32_t now)
{
    if (!tls_voice_session_target(host, port, ctl)) return false;
    pthread_once(&voice_session_once, tls_voice_session_init_once);
    pthread_mutex_lock(&voice_session_lock);
    bool offered = false;
    if (voice_session_valid && (uint32_t)(now - voice_session_saved) >= 60000) {
        mbedtls_ssl_session_free(&voice_session);
        mbedtls_ssl_session_init(&voice_session);
        voice_session_valid = false;
    }
    if (voice_session_valid) offered = mbedtls_ssl_set_session(ssl, &voice_session) == 0;
    pthread_mutex_unlock(&voice_session_lock);
    return offered;
}

static void tls_voice_session_save(mbedtls_ssl_context *ssl, const char *host,
                                   const char *port, const vela_req_ctl_t *ctl,
                                   uint32_t now)
{
    if (!tls_voice_session_target(host, port, ctl) ||
        strcmp(mbedtls_ssl_get_version(ssl), "TLSv1.2") != 0) return;
    pthread_once(&voice_session_once, tls_voice_session_init_once);
    pthread_mutex_lock(&voice_session_lock);
    mbedtls_ssl_session_free(&voice_session);
    mbedtls_ssl_session_init(&voice_session);
    voice_session_valid = mbedtls_ssl_get_session(ssl, &voice_session) == 0;
    voice_session_saved = now;
    pthread_mutex_unlock(&voice_session_lock);
}
#else
static void tls_voice_session_clear(void) {}
#endif

/* ── TLS handshake forensics (diagnostic builds only) ─────────── */
#ifdef CONFIG_AI_AGENT_TLS_FORENSICS

/* Root-cause instrumentation for the intermittent handshake failures
 * (-0x4380 RSA verify failed / -0x3ae2 ASN.1 parse failed).
 *
 * One capture slot serves the first handshake that starts while the slot
 * is free.  The raw BIO receive stream is copied into two independent
 * heap buffers plus one send copy; when the handshake fails with a crypto
 * error the evidence set is written to /data/forensics/tls-<n>/:
 *
 *   cert.der          peer leaf certificate exactly as parsed on board
 *   ske-in-msg.bin    ssl->in_msg tail at the failure point
 *   stream-rx-a.bin   receive stream, copy A (captured at recv time)
 *   stream-rx-b.bin   receive stream, copy B (captured at recv time)
 *   stream-tx.bin     bytes handed to send during the handshake
 *   bundle.b64        all binaries in one base64 container (serial copy)
 *   meta.txt          return code, fingerprints, addresses, randoms
 *
 * Offline comparison of these copies separates "bytes arrived corrupt
 * from the socket" (capture corrupt) from "bytes corrupted inside mbedTLS
 * heap residency after recv" (capture clean, ssl-side data corrupt).
 * Canary words around each copy catch a corruptor that also hits the
 * capture itself.
 *
 * Secret whitelist: only the peer certificate chain, the failing
 * handshake message, the handshake randoms (public by definition) and
 * the handshake BIO stream are recorded.  master_secret, keys and
 * credentials are never touched. */

#define FORENSIC_DIR     "/data/forensics"
#define FORENSIC_RX_CAP  16384
#define FORENSIC_TX_CAP  4096
#define FORENSIC_CANARY  0xC0FFEE01u
#define FORENSIC_KEEP    10

typedef struct {
    int state;                /* 0 idle, 1 capturing, -1 disabled */
    uint8_t *rx_a, *rx_b, *tx;
    uint32_t *can0, *can1, *can2, *can3;
    size_t rx_len, tx_len;
    int rx_ovf, tx_ovf;
    unsigned seq;
} forensics_t;

static forensics_t s_frn;
static pthread_mutex_t s_frn_lock = PTHREAD_MUTEX_INITIALIZER;

static const char* frn_region(uintptr_t a)
{
    /* STM32H750 memory map: AXI SRAM 512K @0x24000000, SRAM1/2 256K +
     * SRAM3 32K @0x30000000, SRAM4 64K @0x38000000, DTCM 128K @0x20000000,
     * SDRAM 8M @0xd0000000 (framebuffer + 6M heap at +2M). */
    if (a >= 0xd0000000u && a < 0xd8000000u) return "SDRAM";
    if (a >= 0x24000000u && a < 0x24080000u) return "AXISRAM";
    if (a >= 0x30000000u && a < 0x30048000u) return "SRAM123";
    if (a >= 0x38000000u && a < 0x38010000u) return "SRAM4";
    if (a >= 0x20000000u && a < 0x20020000u) return "DTCM";
    return "other";
}

static void frn_sha256_hex(const unsigned char* p, size_t n, char out[65])
{
    unsigned char d[32];
    const mbedtls_md_info_t* mi = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (p == NULL || n == 0 || mi == NULL || mbedtls_md(mi, p, n, d) != 0) {
        snprintf(out, 65, "-");
        return;
    }
    for (int i = 0; i < 32; i++) {
        snprintf(out + 2 * i, 3, "%02x", d[i]);
    }
}

static bool frn_begin(void)
{
    pthread_mutex_lock(&s_frn_lock);
    if (s_frn.state != 0) {
        if (s_frn.state == 1) {
            syslog(LOG_WARNING,
                "[%s] forensics: capture slot busy, handshake proceeds uncaptured\n",
                TAG);
        }
        pthread_mutex_unlock(&s_frn_lock);
        return false;
    }
    if (s_frn.rx_a == NULL) {
        uint8_t* blk = malloc(2 * FORENSIC_RX_CAP + FORENSIC_TX_CAP +
                              4 * sizeof(uint32_t));
        if (blk == NULL) {
            syslog(LOG_WARNING, "[%s] forensics capture alloc failed, disabled\n", TAG);
            s_frn.state = -1;
            pthread_mutex_unlock(&s_frn_lock);
            return false;
        }
        s_frn.can0 = (uint32_t*)blk;
        s_frn.rx_a = blk + sizeof(uint32_t);
        s_frn.can1 = (uint32_t*)(s_frn.rx_a + FORENSIC_RX_CAP);
        s_frn.rx_b = (uint8_t*)s_frn.can1 + sizeof(uint32_t);
        s_frn.can2 = (uint32_t*)(s_frn.rx_b + FORENSIC_RX_CAP);
        s_frn.tx = (uint8_t*)s_frn.can2 + sizeof(uint32_t);
        s_frn.can3 = (uint32_t*)(s_frn.tx + FORENSIC_TX_CAP);
        *s_frn.can0 = *s_frn.can1 = *s_frn.can2 = *s_frn.can3 = FORENSIC_CANARY;
        syslog(LOG_INFO, "[%s] forensics capture buffer at %s\n", TAG,
            frn_region((uintptr_t)s_frn.rx_a));
    }
    s_frn.rx_len = s_frn.tx_len = 0;
    s_frn.rx_ovf = s_frn.tx_ovf = 0;
    s_frn.state = 1;
    pthread_mutex_unlock(&s_frn_lock);
    return true;
}

static void frn_end(void)
{
    pthread_mutex_lock(&s_frn_lock);
    if (s_frn.state == 1) {
        s_frn.state = 0;
    }
    pthread_mutex_unlock(&s_frn_lock);
}

/* Both copies are written with independent memcpy calls so a corruptor
 * striking mid-copy leaves them divergent instead of identically wrong. */
static void frn_note_rx(const unsigned char* buf, size_t len)
{
    if (s_frn.state != 1) return;
    size_t room = FORENSIC_RX_CAP - s_frn.rx_len;
    if (len > room) {
        memcpy(s_frn.rx_a + s_frn.rx_len, buf, room);
        memcpy(s_frn.rx_b + s_frn.rx_len, buf, room);
        s_frn.rx_len = FORENSIC_RX_CAP;
        s_frn.rx_ovf = 1;
        return;
    }
    memcpy(s_frn.rx_a + s_frn.rx_len, buf, len);
    memcpy(s_frn.rx_b + s_frn.rx_len, buf, len);
    s_frn.rx_len += len;
}

static void frn_note_tx(const unsigned char* buf, size_t len)
{
    if (s_frn.state != 1) return;
    size_t room = FORENSIC_TX_CAP - s_frn.tx_len;
    if (len > room) {
        memcpy(s_frn.tx + s_frn.tx_len, buf, room);
        s_frn.tx_len = FORENSIC_TX_CAP;
        s_frn.tx_ovf = 1;
        return;
    }
    memcpy(s_frn.tx + s_frn.tx_len, buf, len);
    s_frn.tx_len += len;
}

static bool frn_canaries_ok(void)
{
    return s_frn.rx_a != NULL &&
        *s_frn.can0 == FORENSIC_CANARY && *s_frn.can1 == FORENSIC_CANARY &&
        *s_frn.can2 == FORENSIC_CANARY && *s_frn.can3 == FORENSIC_CANARY;
}

static void frn_write_file(const char* dir, const char* name,
                           const void* p, size_t n)
{
    if (p == NULL || n == 0) return;
    char path[64];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE* f = fopen(path, "w");
    if (f == NULL) {
        syslog(LOG_WARNING, "[%s] forensics write %s failed\n", TAG, path);
        return;
    }
    fwrite(p, 1, n, f);
    fclose(f);
}

static void frn_write_b64(FILE* f, const unsigned char* p, size_t n)
{
    static const char t[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    char line[65];
    size_t i = 0;
    while (i < n) {
        size_t take = n - i > 48 ? 48 : n - i;
        size_t o = 0;
        for (size_t j = 0; j < take; j += 3) {
            uint32_t v = (uint32_t)p[i + j] << 16;
            if (j + 1 < take) v |= (uint32_t)p[i + j + 1] << 8;
            if (j + 2 < take) v |= (uint32_t)p[i + j + 2];
            line[o++] = t[(v >> 18) & 63];
            line[o++] = t[(v >> 12) & 63];
            line[o++] = (j + 1 < take) ? t[(v >> 6) & 63] : '=';
            line[o++] = (j + 2 < take) ? t[v & 63] : '=';
        }
        line[o] = '\0';
        fprintf(f, "%s\n", line);
        i += take;
    }
}

static void frn_dump(mbedtls_ssl_context* ssl, int ret, const char* host)
{
    pthread_mutex_lock(&s_frn_lock);
    if (s_frn.state != 1) {
        /* The failing connection does not own the capture slot. */
        pthread_mutex_unlock(&s_frn_lock);
        return;
    }
    const mbedtls_x509_crt* cert = ssl != NULL ? mbedtls_ssl_get_peer_cert(ssl) : NULL;
    const unsigned char* ske = ssl != NULL ? ssl->in_msg : NULL;
    size_t ske_len = ssl != NULL ? ssl->in_msglen : 0;
    /* Client/server randoms are not read here (ssl->handshake is an
     * opaque type from the public headers); the offline verifier
     * recovers both randoms from the captured ClientHello/ServerHello
     * records in the streams. */
    size_t cert_len = cert != NULL ? cert->raw.len : 0;
    const unsigned char* cert_p = cert != NULL ? cert->raw.p : NULL;

    bool canary = frn_canaries_ok();
    bool rx_eq = s_frn.rx_len > 0 &&
        memcmp(s_frn.rx_a, s_frn.rx_b, s_frn.rx_len) == 0;
    char cert_h[65], ske_h[65], rx_h[65];
    frn_sha256_hex(cert_p, cert_len, cert_h);
    frn_sha256_hex(ske, ske_len, ske_h);
    frn_sha256_hex(s_frn.rx_a, s_frn.rx_len, rx_h);

    syslog(LOG_ERR,
        "[%s] FORENSIC seq=%u ret=-0x%04x host=%s cert=%s%.16s in_msg=%u "
        "rx=%u%s tx=%u%s rxAB=%s canary=%s\n",
        TAG, s_frn.seq, -ret, host,
        cert_len ? "" : "none:", cert_len ? cert_h : "-",
        (unsigned)ske_len,
        (unsigned)s_frn.rx_len, s_frn.rx_ovf ? "+ovf" : "",
        (unsigned)s_frn.tx_len, s_frn.tx_ovf ? "+ovf" : "",
        rx_eq ? "match" : "DIFFER", canary ? "ok" : "BROKEN");

    char dir[48];
    snprintf(dir, sizeof(dir), FORENSIC_DIR "/tls-%u", s_frn.seq % FORENSIC_KEEP);
    mkdir(FORENSIC_DIR, 0755);
    mkdir(dir, 0755);

    char path[64];
    FILE* f;
    snprintf(path, sizeof(path), "%s/meta.txt", dir);
    if ((f = fopen(path, "w")) != NULL) {
        fprintf(f, "seq %u\nret -0x%04x\nhost %s\ntime %ld\n",
            s_frn.seq, -ret, host, (long)time(NULL));
        if (ssl != NULL) {
            fprintf(f, "tls %s %s\n", mbedtls_ssl_get_version(ssl),
                mbedtls_ssl_get_ciphersuite(ssl));
        }
        fprintf(f, "cert_len %u\ncert_sha256 %s\n",
            (unsigned)cert_len, cert_h);
        fprintf(f, "in_msglen %u\nin_msg_sha256 %s\n",
            (unsigned)ske_len, ske_h);
        fprintf(f, "rx_len %u rx_ovf %d rx_sha256 %s rx_a_eq_rx_b %d\n",
            (unsigned)s_frn.rx_len, s_frn.rx_ovf, rx_h, rx_eq ? 1 : 0);
        fprintf(f, "tx_len %u tx_ovf %d\n",
            (unsigned)s_frn.tx_len, s_frn.tx_ovf);
        fprintf(f, "randbytes recovered-offline from captured ClientHello/ServerHello\n");
        fprintf(f, "ptr_ssl %p %s\n", (void*)ssl, frn_region((uintptr_t)ssl));
        if (ssl != NULL) {
            fprintf(f, "ptr_in_buf %p %s\n", (void*)ssl->in_buf,
                frn_region((uintptr_t)ssl->in_buf));
            fprintf(f, "ptr_in_msg %p %s\n", (void*)ssl->in_msg,
                frn_region((uintptr_t)ssl->in_msg));
            fprintf(f, "ptr_out_buf %p %s\n", (void*)ssl->out_buf,
                frn_region((uintptr_t)ssl->out_buf));
        }
        fprintf(f, "ptr_cert %p %s\n", (void*)cert_p,
            cert_p ? frn_region((uintptr_t)cert_p) : "-");
        fprintf(f, "ptr_capture %p %s\n", (void*)s_frn.rx_a,
            frn_region((uintptr_t)s_frn.rx_a));
        fprintf(f, "secrets none; whitelist cert chain, failing message, "
            "randoms, handshake BIO stream\n");
        fclose(f);
    }

    frn_write_file(dir, "cert.der", cert_p, cert_len);
    frn_write_file(dir, "ske-in-msg.bin", ske, ske_len);
    frn_write_file(dir, "stream-rx-a.bin", s_frn.rx_a, s_frn.rx_len);
    frn_write_file(dir, "stream-rx-b.bin", s_frn.rx_b, s_frn.rx_len);
    frn_write_file(dir, "stream-tx.bin", s_frn.tx, s_frn.tx_len);

    /* Single base64 container so a plain serial `cat` retrieves
     * everything without binary-unaware terminals. */
    size_t payload =
        128 + cert_len + ske_len + 2 * s_frn.rx_len + s_frn.tx_len;
    uint8_t* buf = malloc(payload);
    if (buf != NULL) {
        size_t off = (size_t)snprintf((char*)buf, payload,
            "VGTLS1\nfile cert.der %u\nfile ske-in-msg.bin %u\n"
            "file stream-rx-a.bin %u\nfile stream-rx-b.bin %u\n"
            "file stream-tx.bin %u\ndata\n",
            (unsigned)cert_len, (unsigned)ske_len,
            (unsigned)s_frn.rx_len, (unsigned)s_frn.rx_len,
            (unsigned)s_frn.tx_len);
        memcpy(buf + off, cert_p, cert_len); off += cert_len;
        memcpy(buf + off, ske, ske_len); off += ske_len;
        memcpy(buf + off, s_frn.rx_a, s_frn.rx_len); off += s_frn.rx_len;
        memcpy(buf + off, s_frn.rx_b, s_frn.rx_len); off += s_frn.rx_len;
        memcpy(buf + off, s_frn.tx, s_frn.tx_len); off += s_frn.tx_len;
        snprintf(path, sizeof(path), "%s/bundle.b64", dir);
        if ((f = fopen(path, "w")) != NULL) {
            frn_write_b64(f, buf, off);
            fclose(f);
        }
        free(buf);
    }
    s_frn.seq++;
    pthread_mutex_unlock(&s_frn_lock);
}

static int tls_forensic_send(void* arg, const unsigned char* buf, size_t len)
{
    tls_ctx_t* ctx = arg;
    frn_note_tx(buf, len);
    return ctx->fr_send(ctx->fr_bio, buf, len);
}

static int tls_forensic_recv(void* arg, unsigned char* buf, size_t len)
{
    tls_ctx_t* ctx = arg;
    int r = ctx->fr_recv(ctx->fr_bio, buf, len);
    if (r > 0) {
        frn_note_rx(buf, (size_t)r);
    }
    return r;
}
#endif /* CONFIG_AI_AGENT_TLS_FORENSICS */

/* ── Diagnostic TLS pool (阶段2A A/B experiment) ───────────────── */
#ifdef CONFIG_AI_AGENT_TLS_POOL

/* Route every mbedTLS allocation through a dedicated static first-fit
 * pool with canaries instead of the general heap, where the 16 KB TLS
 * buffers land in SDRAM.  If the -0x4380 handshake failures disappear
 * while mbedTLS data lives in this AXI SRAM pool, the corruption is
 * confined to the general-heap region used for those buffers; if the
 * failures persist, the corruptor follows the data and is not bound to
 * one memory region.  Requires MBEDTLS_PLATFORM_MEMORY. */

/* Header must cover pool_block_t on any word size (32-bit target: 16,
 * 64-bit host tests: 24) and keep the payload 8-byte aligned. */
#define POOL_HDR_SIZE    (((sizeof(size_t) + sizeof(uint32_t) + \
                            sizeof(void*) + 7u) & ~(size_t)7u))
#define POOL_MAGIC_USED  0x0BAD0BADu
#define POOL_MAGIC_FREE  0xF7EEF7EEu
#define POOL_CANARY_TAIL 0x7AC0FFEEu
#define POOL_MIN_SPLIT   32

typedef struct pool_block {
    size_t size;              /* payload size, header excluded */
    uint32_t magic;
    struct pool_block* next;  /* address-ordered free list */
} pool_block_t;

static uint8_t s_poolmem[CONFIG_AI_AGENT_TLS_POOL_SIZE]
    __attribute__((aligned(32)));
static pool_block_t* s_pool_free;
static pthread_mutex_t s_tlsalloc_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t s_pool_used, s_pool_peak, s_pool_fail, s_pool_corrupt;
static pthread_once_t s_pool_once = PTHREAD_ONCE_INIT;

static void tls_pool_corruption(const char* what, pool_block_t* blk,
                                const void* ptr)
{
    s_pool_corrupt++;
    syslog(LOG_ERR,
        "[%s] POOL CORRUPTION %s blk=%p size=%u ptr=%p arena+%u "
        "(used=%u peak=%u fail=%u corrupt=%u)\n",
        TAG, what, (void*)blk, (unsigned)blk->size, ptr,
        (unsigned)((uint8_t*)blk - s_poolmem),
        (unsigned)s_pool_used, (unsigned)s_pool_peak,
        (unsigned)s_pool_fail, (unsigned)s_pool_corrupt);
}

static void* tls_pool_alloc(size_t n)
{
    if (n == 0 || n > CONFIG_AI_AGENT_TLS_POOL_SIZE) {
        return NULL;
    }
    n = (n + 7u) & ~(size_t)7u;
    pthread_mutex_lock(&s_tlsalloc_lock);
    pool_block_t* prev = NULL;
    pool_block_t* blk = s_pool_free;
    /* header + payload + tail canary + pad to keep 8-byte payload
     * alignment across splits */
    size_t need = n + POOL_HDR_SIZE + 8;
    while (blk != NULL) {
        if (blk->magic != POOL_MAGIC_FREE) {
            tls_pool_corruption("free-list magic", blk, NULL);
            pthread_mutex_unlock(&s_tlsalloc_lock);
            return NULL;
        }
        /* blk->size covers payload plus tail canary and pad; the request
         * needs n payload bytes plus that 8-byte tail overhead. */
        if (blk->size >= n + 8) {
            break;
        }
        prev = blk;
        blk = blk->next;
    }
    if (blk == NULL) {
        s_pool_fail++;
        pthread_mutex_unlock(&s_tlsalloc_lock);
        syslog(LOG_ERR, "[%s] TLS pool exhausted (need %u, used %u peak %u)\n",
            TAG, (unsigned)need, (unsigned)s_pool_used, (unsigned)s_pool_peak);
        return NULL;
    }
    size_t have = blk->size;   /* payload+tail space in this free block */
    if (have >= need - POOL_HDR_SIZE - 8 + POOL_HDR_SIZE + 8 + POOL_MIN_SPLIT) {
        /* split off a remainder that can hold a minimal block */
        uint8_t* split = (uint8_t*)blk + POOL_HDR_SIZE + n + 8;
        pool_block_t* nb = (pool_block_t*)split;
        nb->size = have - (n + 8) - POOL_HDR_SIZE - 8;
        nb->magic = POOL_MAGIC_FREE;
        nb->next = blk->next;
        blk->next = nb;
    }
    blk->size = n + 8;
    /* Unlink from the free list. */
    if (prev != NULL) prev->next = blk->next;
    else s_pool_free = blk->next;
    blk->magic = POOL_MAGIC_USED;
    blk->next = NULL;
    uint8_t* payload = (uint8_t*)blk + POOL_HDR_SIZE;
    *(uint32_t*)(payload + n) = POOL_CANARY_TAIL;
    s_pool_used += blk->size + POOL_HDR_SIZE;
    if (s_pool_used > s_pool_peak) s_pool_peak = s_pool_used;
    pthread_mutex_unlock(&s_tlsalloc_lock);
    return payload;
}

static void tls_pool_free(void* ptr)
{
    if (ptr == NULL) return;
    pool_block_t* blk = (pool_block_t*)((uint8_t*)ptr - POOL_HDR_SIZE);
    pthread_mutex_lock(&s_tlsalloc_lock);
    if ((uint8_t*)blk < s_poolmem ||
        (uint8_t*)blk >= s_poolmem + CONFIG_AI_AGENT_TLS_POOL_SIZE) {
        s_pool_corrupt++;
        syslog(LOG_ERR, "[%s] POOL CORRUPTION free outside arena ptr=%p\n",
            TAG, ptr);
        pthread_mutex_unlock(&s_tlsalloc_lock);
        return;
    }
    if (blk->magic != POOL_MAGIC_USED) {
        tls_pool_corruption("header magic on free", blk, ptr);
        pthread_mutex_unlock(&s_tlsalloc_lock);
        return;
    }
    uint8_t* payload = (uint8_t*)blk + POOL_HDR_SIZE;
    /* blk->size = payload (8-aligned) + tail canary + pad */
    if (*(uint32_t*)(payload + blk->size - 8) != POOL_CANARY_TAIL) {
        tls_pool_corruption("tail canary", blk, ptr);
    }
    s_pool_used -= blk->size + POOL_HDR_SIZE;
    blk->magic = POOL_MAGIC_FREE;
    /* Reinsert into the address-ordered free list and coalesce. */
    pool_block_t* prev = NULL;
    pool_block_t* cur = s_pool_free;
    while (cur != NULL && cur < blk) {
        prev = cur;
        cur = cur->next;
    }
    blk->next = cur;
    if (prev != NULL) prev->next = blk;
    else s_pool_free = blk;
    if (cur != NULL &&
        (uint8_t*)blk + POOL_HDR_SIZE + blk->size == (uint8_t*)cur) {
        blk->size += POOL_HDR_SIZE + cur->size;
        blk->next = cur->next;
    }
    if (prev != NULL &&
        (uint8_t*)prev + POOL_HDR_SIZE + prev->size == (uint8_t*)blk) {
        prev->size += POOL_HDR_SIZE + blk->size;
        prev->next = blk->next;
    }
    pthread_mutex_unlock(&s_tlsalloc_lock);
}

static void* tls_pool_calloc(size_t n, size_t sz)
{
    if (sz != 0 && n > (size_t)-1 / sz) return NULL;
    void* p = tls_pool_alloc(n * sz);
    if (p != NULL) memset(p, 0, n * sz);
    return p;
}

static void tls_pool_install_once(void)
{
    pool_block_t* blk = (pool_block_t*)s_poolmem;
    blk->size = CONFIG_AI_AGENT_TLS_POOL_SIZE - POOL_HDR_SIZE - 8;
    blk->magic = POOL_MAGIC_FREE;
    blk->next = NULL;
    s_pool_free = blk;
    mbedtls_platform_set_calloc_free(tls_pool_calloc, tls_pool_free);
    syslog(LOG_INFO,
        "[%s] TLS pool installed: %u bytes at %p, mbedTLS allocations "
        "redirected from the general heap\n",
        TAG, CONFIG_AI_AGENT_TLS_POOL_SIZE, (void*)s_poolmem);
}

static void tls_pool_ensure_installed(void)
{
    pthread_once(&s_pool_once, tls_pool_install_once);
}
#else
static void tls_pool_ensure_installed(void) {}
#endif /* CONFIG_AI_AGENT_TLS_POOL */

static int tls_ctx_connect(tls_ctx_t* ctx, const char* host, const char* port,
    agent_llm_timing_t* timing, const vela_req_ctl_t *ctl)
{
    int ret;
    uint32_t mark = agent_mono_ms();

    tls_pool_ensure_installed();

    ctx->abort_on_close = ctl != NULL;
    ctx->send_pressure = 0;
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
        if (timing != NULL) {
            timing->tls_setup_ms = agent_lap_ms(&mark);
        }
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

    if (timing != NULL) {
        timing->tls_setup_ms = agent_lap_ms(&mark);
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

    if (http_proxy_is_enabled()) {
        if (timing != NULL) {
            /* The tunnel also covers reaching the proxy, so the phase is
             * still one combined connect.  The LLM layer marks the proxy
             * path separately when it sees http_proxy_is_enabled(). */

            timing->fused_dns_tcp = true;
        }

        /* The proxy tunnel has a separate blocking contract.  Controlled
         * callers must fail explicitly rather than silently lose the budget. */
        if (ctl != NULL) {
            return VELA_TLS_ERR_CONNECT;
        }
        int tunnel_fd = proxy_open_tunnel(host, atoi(port), 30000);
        if (tunnel_fd < 0) {
            syslog(LOG_ERR, "[%s] proxy tunnel to %s:%s failed\n",
                TAG, host, port);
            return VELA_TLS_ERR_CONNECT;
        }
        ctx->net.fd = tunnel_fd;
        syslog(LOG_INFO, "[%s] Using proxy tunnel fd=%d for %s:%s\n",
            TAG, tunnel_fd, host, port);
    } else {
        ret = ctl != NULL ? tls_connect_controlled(ctx, host, port, ctl)
                          : mbedtls_net_connect(&ctx->net, host, port,
                                MBEDTLS_NET_PROTO_TCP);

        /* The connect time is recorded before looking at the result.  It
         * used to be recorded only on success, so a connect that burned 81 s
         * of SYN retries before failing left dns_tcp at na and the wait
         * appeared only in the http total.  A failed connect is still a
         * measured connect. */

        if (timing != NULL) {
            timing->dns_tcp_ms = agent_lap_ms(&mark);
            timing->fused_dns_tcp = true;
        }

        if (ret != 0) {
            syslog(LOG_ERR, "[%s] net_connect %s:%s ret=0x%x\n",
                TAG, host, port, -ret);
            return ctl != NULL ? ret : VELA_TLS_ERR_CONNECT;
        }
    }

    if (timing != NULL && timing->dns_tcp_ms == AGENT_T_NA) {
        /* Proxy path: the tunnel call above also covers reaching the proxy,
         * so it is one combined connect. */

        timing->dns_tcp_ms = agent_lap_ms(&mark);
        timing->fused_dns_tcp = true;
    }

    /* Force blocking mode and set socket-level read timeout.
     * This avoids using select()/poll() inside mbedtls_net_recv_timeout
     * which returns MBEDTLS_ERR_NET_POLL_FAILED (-0x0047) on NuttX/QEMU.
     * Timeout is configured via AGENT_LLM_SOCKET_TIMEOUT_SEC in
     * agent_config.h.  LLM APIs (especially kimi with thinking mode)
     * can take over 60s for long responses. */
    if (ctl == NULL) {
        mbedtls_net_set_block(&ctx->net);
    } else if (mbedtls_net_set_nonblock(&ctx->net) != 0) {
        return VELA_TLS_ERR_CONNECT;
    }
    if (ctx->net.fd >= 0) {
#ifdef CONFIG_AI_AGENT_NET_RPMSG
        int read_timeout_sec = network_get_read_timeout();
        struct timeval tv = { .tv_sec = read_timeout_sec, .tv_usec = 0 };
#else
        struct timeval tv = { .tv_sec = AGENT_LLM_SOCKET_TIMEOUT_SEC,
            .tv_usec = 0 };
#endif
        setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

#ifdef CONFIG_AI_AGENT_NET_RPMSG
        {
            int timeout_sec = network_get_connect_timeout();
            struct timeval ctv = { .tv_sec = timeout_sec, .tv_usec = 0 };
            setsockopt(ctx->net.fd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));
        }
#endif
    }

    /* SSL config: client, TLS, default ciphersuites */
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

    /* Read timeout is handled at the socket level via SO_RCVTIMEO,
     * not via mbedtls_ssl_conf_read_timeout + mbedtls_net_recv_timeout,
     * because select()/poll() inside mbedtls_net_recv_timeout fails on
     * NuttX/QEMU with MBEDTLS_ERR_NET_POLL_FAILED (-0x0047). */

    if ((ret = mbedtls_ssl_setup(&ctx->ssl, &ctx->cfg)) != 0) {
        syslog(LOG_ERR, "[%s] ssl_setup ret=0x%x\n", TAG, -ret);
        return VELA_TLS_ERR_HANDSHAKE;
    }

    if ((ret = mbedtls_ssl_set_hostname(&ctx->ssl, host)) != 0) {
        syslog(LOG_ERR, "[%s] ssl_set_hostname ret=0x%x\n", TAG, -ret);
        return VELA_TLS_ERR_HANDSHAKE;
    }

#if defined(CONFIG_VG_HMI) && defined(CONFIG_VG_VOICE_TLS_CACHE) && defined(MBEDTLS_SSL_CLI_C)
    bool session_offered = tls_voice_session_offer(&ctx->ssl, host, port, ctl, agent_mono_ms());
#endif

    /* Use blocking recv — no select()/poll().  The read
     * timeout is enforced by the SO_RCVTIMEO socket option set above. */
#ifdef CONFIG_AI_AGENT_TLS_FORENSICS
    ctx->fr_owned = frn_begin();
    if (ctx->fr_owned) {
        ctx->fr_send = (ctl != NULL) ? tls_controlled_send : mbedtls_net_send;
        ctx->fr_recv = (ctl != NULL) ? tls_controlled_recv : mbedtls_net_recv;
        ctx->fr_bio = (ctl != NULL) ? (void*)ctx : (void*)&ctx->net;
        mbedtls_ssl_set_bio(&ctx->ssl, ctx,
            tls_forensic_send, tls_forensic_recv, NULL);
    } else
#endif
    if (ctl != NULL) {
        mbedtls_ssl_set_bio(&ctx->ssl, ctx,
            tls_controlled_send, tls_controlled_recv, NULL);
    } else {
        mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net,
            mbedtls_net_send, mbedtls_net_recv, NULL);
    }

    /* Timer for the handshake starts here, after configuration.  Setup cost
     * is already accounted for in tls_setup_ms so the two do not overlap. */

    mark = agent_mono_ms();

    /* Handshake */
    for (;;) {
        if (ctl != NULL) {
            if (vela_req_cancelled(ctl)) {
#ifdef CONFIG_AI_AGENT_TLS_FORENSICS
                if (ctx->fr_owned) { frn_end(); ctx->fr_owned = false; }
#endif
                return VELA_TLS_ERR_CANCELLED;
            }
            if (vela_req_budget_ms(ctl, agent_mono_ms()) == 0) {
#ifdef CONFIG_AI_AGENT_TLS_FORENSICS
                if (ctx->fr_owned) { frn_end(); ctx->fr_owned = false; }
#endif
                return VELA_TLS_ERR_TIMEOUT;
            }
        }
        ret = mbedtls_ssl_handshake(&ctx->ssl);
        if (ret == 0) {
            break;
        }
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
#if defined(MBEDTLS_ERROR_C)
            char err_buf[128];
            mbedtls_strerror(ret, err_buf, sizeof(err_buf));
            syslog(LOG_ERR, "[%s] ssl_handshake ret=-0x%04x: %s\n", TAG, -ret, err_buf);
#else
            syslog(LOG_ERR, "[%s] ssl_handshake ret=-0x%04x\n", TAG, -ret);
#endif
#ifdef CONFIG_AI_AGENT_TLS_FORENSICS
            if (ctx->fr_owned) {
                frn_dump(&ctx->ssl, ret, host);
                frn_end();
                ctx->fr_owned = false;
            }
#endif
            /* Log if it was a fatal alert */
            if (ret == MBEDTLS_ERR_SSL_FATAL_ALERT_MESSAGE) {
                syslog(LOG_ERR, "[%s] Server sent fatal alert message\n", TAG);
            }

            /* A handshake that failed after waiting is still a measured
             * handshake; leaving tls at na would put its time nowhere. */

            if (timing != NULL) {
                timing->tls_ms = agent_lap_ms(&mark);
            }
#if defined(CONFIG_VG_HMI) && defined(CONFIG_VG_VOICE_TLS_CACHE) && defined(MBEDTLS_SSL_CLI_C)
            if (session_offered) tls_voice_session_clear();
#endif
            return VELA_TLS_ERR_HANDSHAKE;
        }
        if (ctl != NULL) {
            ret = tls_wait_socket(ctx->net.fd,
                ret == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, ctl);
            if (ret != 0) {
#ifdef CONFIG_AI_AGENT_TLS_FORENSICS
                if (ctx->fr_owned) { frn_end(); ctx->fr_owned = false; }
#endif
                return ret;
            }
        }
    }

#ifdef CONFIG_AI_AGENT_TLS_FORENSICS
    if (ctx->fr_owned) { frn_end(); ctx->fr_owned = false; }
#endif

#if defined(CONFIG_VG_HMI) && defined(CONFIG_VG_VOICE_TLS_CACHE) && defined(MBEDTLS_SSL_CLI_C)
    uint32_t handshake_ms = agent_elapsed_ms(mark, agent_mono_ms());
#endif
    if (timing != NULL) {
        timing->tls_ms = agent_lap_ms(&mark);
    }

#if defined(CONFIG_VG_HMI) && defined(CONFIG_VG_VOICE_TLS_CACHE) && defined(MBEDTLS_SSL_CLI_C)
    if (tls_voice_session_target(host, port, ctl)) {
        syslog(LOG_INFO, "[%s] voice session offered=%u handshake_ms=%u\n", TAG,
            session_offered, (unsigned)handshake_ms);
        tls_voice_session_save(&ctx->ssl, host, port, ctl, agent_mono_ms());
    }
#endif

    syslog(LOG_INFO, "[%s] Handshake OK: %s / %s\n", TAG, mbedtls_ssl_get_version(&ctx->ssl),
        mbedtls_ssl_get_ciphersuite(&ctx->ssl));

    return 0;
}

/* ── HTTP/1.1 framing ────────────────────────────────────────── */

/* mbedtls_ssl_write() wrapper that survives a full socket send buffer.
 *
 * tls_ctx_connect() puts the socket in blocking mode and leaves it there, and
 * mbedtls' net_would_block() refuses to report a would-block condition on a
 * blocking socket ("Never return 'WOULD BLOCK' on a blocking socket").  A
 * send() that meets a full TCP send buffer therefore reaches us as
 * MBEDTLS_ERR_NET_SEND_FAILED instead of MBEDTLS_ERR_SSL_WANT_WRITE, which
 * used to abort a ~16 KB LLM request body right after a healthy handshake.
 *
 * Keep each record small and treat a short or failed send as retryable for a
 * bounded budget.  Retrying is safe: mbedtls keeps the unsent tail in
 * ssl->out_left and flushes it before encrypting anything new. */

#define TLS_WRITE_CHUNK      4096
#define TLS_WRITE_ATTEMPTS   64
#define TLS_WRITE_BACKOFF_US 20000

/* A send that failed because the socket is gone must not be retried: the
 * peer closed the connection and every retry waits for a timeout that
 * cannot succeed.  NuttX reports these as ENOTCONN, EPIPE, ECONNRESET or
 * EBADF.  ENOMEM is the opposite case: the stack ran out of packet buffers
 * and the send becomes possible again once the peer acknowledges. */
static bool tls_send_error_is_fatal(int err)
{
    switch (err)
    {
        case ENOTCONN:
        case EPIPE:
        case ECONNRESET:
        case EBADF:
        case ENOTSOCK:
            return true;
        default:
            return false;
    }
}

static int tls_ssl_write_all(tls_ctx_t* ctx, const char* what,
    const unsigned char* buf, size_t len)
{
    size_t off = 0;
    int attempts = 0;

    while (off < len) {
        size_t chunk = len - off;
        int ret;

        if (chunk > TLS_WRITE_CHUNK) {
            chunk = TLS_WRITE_CHUNK;
        }

        ret = mbedtls_ssl_write(&ctx->ssl, buf + off, chunk);
        if (ret > 0) {
            off += (size_t)ret;
            attempts = 0;
            continue;
        }

        if (ret == 0) {
            syslog(LOG_ERR, "[%s] %s write stalled at %u/%u\n",
                TAG, what, (unsigned)off, (unsigned)len);
            return VELA_TLS_ERR_WRITE;
        }

        attempts++;
        {
            int err = errno;

            /* The connection is gone: retrying only burns the budget. */

            if (tls_send_error_is_fatal(err)) {
                syslog(LOG_ERR,
                    "[%s] %s write failed at %u/%u ret=-0x%04x errno=%d (connection gone)\n",
                    TAG, what, (unsigned)off, (unsigned)len, -ret, err);
                return VELA_TLS_ERR_WRITE;
            }

            if (attempts == 1 || attempts >= TLS_WRITE_ATTEMPTS) {
                int fl = (ctx->net.fd >= 0) ? fcntl(ctx->net.fd, F_GETFL) : -1;

                syslog(attempts == 1 ? LOG_WARNING : LOG_ERR,
                    "[%s] %s write retry %d at %u/%u ret=-0x%04x errno=%d fl=0x%x\n",
                    TAG, what, attempts, (unsigned)off, (unsigned)len,
                    -ret, err, (unsigned)fl);
            }
        }

        if (attempts >= TLS_WRITE_ATTEMPTS) {
            return VELA_TLS_ERR_WRITE;
        }

        /* Wait for the socket to take data again.  A closed peer reports an
         * error event immediately instead of after the full backoff. */

        {
            struct pollfd pfd = { .fd = ctx->net.fd, .events = POLLOUT };
            int pr = poll(&pfd, 1, TLS_WRITE_BACKOFF_US / 1000);

            if (pr > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
                syslog(LOG_ERR, "[%s] %s write sees a dead socket at %u/%u\n",
                    TAG, what, (unsigned)off, (unsigned)len);
                return VELA_TLS_ERR_WRITE;
            }
        }
    }

    return 0;
}

/* ── Request byte dump (isolation diagnostics; default off) ──────
 * Prints the exact request-line/header block and a short body prefix
 * so a board failure can be diffed byte-for-byte against a host replay.
 * Credential header values are masked before anything reaches the
 * serial log. */

static int g_req_dump;

void vela_tls_set_req_dump(int on)
{
  g_req_dump = on;
}

static void tls_dump_head(const char *tag, const char *head, size_t len)
{
  char *copy = malloc(len + 1);
  if (copy == NULL)
    return;
  memcpy(copy, head, len);
  copy[len] = '\0';

  char *auth = strcasestr(copy, "Authorization:");
  if (auth != NULL)
    {
      char *val = strchr(auth, ':');
      if (val != NULL)
        {
          val++;
          while (*val == ' ')
            val++;
          while (*val != '\0' && *val != '\r' && *val != '\n')
            *val++ = '*';
        }
    }

  char line[96];
  size_t o = 0;
  for (size_t i = 0; i < len; i++)
    {
      unsigned char c = (unsigned char)copy[i];
      const char *esc = c == '\r' ? "\\r" : c == '\n' ? "\\n" : NULL;
      size_t need = esc ? 2 : (c >= 0x20 && c < 0x7f) ? 1 : 4;
      if (o + need >= sizeof(line))
        {
          line[o] = '\0';
          syslog(LOG_INFO, "[dump:%s] %s\n", tag, line);
          o = 0;
        }
      if (esc != NULL)
        {
          line[o++] = esc[0];
          line[o++] = esc[1];
        }
      else if (c >= 0x20 && c < 0x7f)
        line[o++] = (char)c;
      else
        o += (size_t)snprintf(line + o, sizeof(line) - o, "\\x%02x", c);
    }
  if (o != 0)
    {
      line[o] = '\0';
      syslog(LOG_INFO, "[dump:%s] %s\n", tag, line);
    }
  free(copy);
}

static void tls_dump_hex(const char *tag, const void *bytes, size_t len)
{
  const unsigned char *p = bytes;
  char line[65];
  for (size_t i = 0; i < len; i += 32)
    {
      size_t o = 0;
      for (size_t j = i; j < len && j < i + 32; j++)
        o += (size_t)snprintf(line + o, 3, "%02x", p[j]);
      line[o] = '\0';
      syslog(LOG_INFO, "[dump:%s] %s\n", tag, line);
    }
}

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

    if (g_req_dump)
      {
        tls_dump_head("req", hdr, (size_t)pos);
        if (body != NULL && body_len > 0)
          tls_dump_hex("reqbody", body,
                       g_req_dump >= 2 ? body_len :
                       (body_len < 96 ? body_len : 96));
      }

    /* Write headers */
    if (tls_ssl_write_all(ctx, "hdr",
            (const unsigned char*)hdr, (size_t)pos) != 0) {
        free(hdr);
        return VELA_TLS_ERR_WRITE;
    }

    free(hdr);

    /* Write body */
    if (body && body_len > 0) {
        if (tls_ssl_write_all(ctx, "body",
                (const unsigned char*)body, body_len) != 0) {
            return VELA_TLS_ERR_WRITE;
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

/* Receive progress: a gap at or above this many milliseconds between reads is
 * recorded, up to VELA_TLS_PROGRESS_MAX entries.  Below the gap limit the
 * reply is arriving steadily and needs no per-read trace. */

#define VELA_TLS_PROGRESS_GAP_MS 1000
#define VELA_TLS_PROGRESS_MAX    8

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

/* t0 is the moment the request finished sending, so ttfb_ms covers the
 * server's wait before the first response byte.  The first byte is the
 * status line: this path is not streamed, so it is not a model token. */

static int tls_read_response(tls_ctx_t* ctx, char* resp_buf, size_t resp_cap,
    size_t* out_body_len, bool* out_keep_alive, uint32_t t0,
    agent_llm_timing_t* timing)
{
    /* Use static buffers to avoid heap fragmentation */
    char* raw = tls_raw_acquire();
    size_t raw_len = 0;
    int eof = 0;
    int ret;
    bool first_byte_seen = false;

    /* Receive progress.  A reply that delivers its body in one or two reads
     * leaves gaps at zero and prints nothing. */

    uint32_t last_read_ms = t0;
    uint32_t first_read_ms = 0;
    int gaps = 0;
    int32_t progress_gap[VELA_TLS_PROGRESS_MAX];
    uint32_t progress_at[VELA_TLS_PROGRESS_MAX];
    uint32_t progress_bytes[VELA_TLS_PROGRESS_MAX];

    /* Read until we have the full header (double CRLF) or buffer full */
    while (!eof && raw_len < TLS_RAW_BUF_SIZE - 1) {
        ret = mbedtls_ssl_read(&ctx->ssl,
            (unsigned char*)(raw + raw_len),
            TLS_RAW_BUF_SIZE - 1 - raw_len);
        if (ret > 0 && !first_byte_seen) {
            first_byte_seen = true;
            if (timing != NULL) {
                timing->ttfb_ms = agent_elapsed_ms(t0, agent_mono_ms());
            }
        }

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

    /* Keep reading body.
     *
     * The end condition depends on how the response is framed.  Before this
     * only Content-Length was honoured, so a chunked reply on a keep-alive
     * connection kept reading after its terminating chunk until the socket
     * timed out.  vela_http_body_complete() applies RFC 9112 6.3: chunked
     * ends at the zero chunk, Content-Length ends at its count, and an
     * unframed reply ends when the peer closes.
     *
     * Completeness is tested before every read, including the first, because
     * the initial fragment copied above may already hold the whole body.  A
     * read issued after that would wait for bytes that will never arrive. */

    while (!eof && resp_pos < resp_cap - 1) {
        if (vela_http_body_complete(chunked, content_length, resp_pos,
                resp_buf)) {
            break;
        }
        ret = mbedtls_ssl_read(&ctx->ssl,
            (unsigned char*)(resp_buf + resp_pos),
            resp_cap - 1 - resp_pos);
        if (ret > 0) {
            resp_pos += (size_t)ret;

            /* Receive progress for slow replies.
             *
             * A body that takes minutes can be arriving steadily, arriving in
             * bursts after long silences, or already complete while the caller
             * waits on something else.  Reporting cumulative bytes with the
             * gap since the previous read separates those without logging any
             * content.  Only replies that pass the threshold are traced, so a
             * normal round stays quiet. */

            uint32_t now = agent_mono_ms();

            if (gaps == 0 && first_read_ms == 0) {
                first_read_ms = now;
            }

            /* Idle is measured from the previous read inside this loop, not
             * from t0.  On the first iteration last_read_ms is still t0, so
             * the initial server wait would be counted twice: once as ttfb
             * and again as a body gap.  Seed it at the first byte instead. */

            if (last_read_ms == t0) {
                last_read_ms = now;
            }

            int32_t idle = agent_elapsed_ms(last_read_ms, now);

            if (idle >= VELA_TLS_PROGRESS_GAP_MS) {
                if (gaps < VELA_TLS_PROGRESS_MAX) {
                    progress_gap[gaps] = idle;
                    progress_at[gaps] = agent_elapsed_ms(t0, now);
                    progress_bytes[gaps] = (uint32_t)resp_pos;
                }
                gaps++;
            }

            last_read_ms = now;
        } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            break;
        } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
            break;
        }
    }

    /* One line per slow reply.  deltas says how many gaps were seen, which
     * can exceed the stored entries; the entries are the worst ones seen
     * first, so a truncated list still shows the shape of the wait. */

    if (timing != NULL && gaps > 0) {
        int shown = gaps < VELA_TLS_PROGRESS_MAX ? gaps : VELA_TLS_PROGRESS_MAX;

        syslog(LOG_INFO,
            "[%s] body progress: gaps=%d first_read=%ldms total=%u bytes\n",
            TAG, gaps,
            (long)agent_elapsed_ms(t0, first_read_ms), (unsigned)resp_pos);

        for (int i = 0; i < shown; i++) {
            syslog(LOG_INFO,
                "[%s] body gap %d: idle=%ldms at=%ums bytes=%u\n",
                TAG, i + 1, (long)progress_gap[i], (unsigned)progress_at[i],
                (unsigned)progress_bytes[i]);
        }
    }

    if (vela_http_body_unframed(chunked, content_length) && !eof) {
        /* No length information: the wait can only end by the peer closing
         * or by the socket timeout.  Name it, so an inflated body_ms on such
         * a reply is not read as slow generation. */

        syslog(LOG_WARNING,
            "[%s] response has neither Content-Length nor chunked framing; "
            "body end depends on the peer closing\n", TAG);
    }

    resp_buf[resp_pos] = '\0';
    tls_raw_release(raw);

    if (timing != NULL && first_byte_seen) {
        /* Measured from the first byte, so headers and body form one span
         * and never overlap ttfb_ms. */

        timing->body_ms = agent_elapsed_ms(t0, agent_mono_ms())
            - timing->ttfb_ms;
        if (timing->body_ms < 0) {
            timing->body_ms = AGENT_T_NA;
        }
    }


    /* A body that filled the caller's buffer was cut off mid-stream.  The
     * caller only sees a short string, so a JSON consumer reports a syntax
     * error that does not exist in the server's reply.  Name the real cause
     * here, where the capacity is still known. */
    if (resp_pos >= resp_cap - 1) {
        syslog(LOG_ERR, "[%s] response body truncated at %zu bytes "
               "(buf cap %zu): reply larger than buffer\n",
               TAG, resp_pos, resp_cap);
    }

    /* Chunked decode */
    if (chunked) {
        resp_pos = decode_chunked(resp_buf, resp_pos);
        resp_buf[resp_pos] = '\0';
    }

    if (out_body_len)
        *out_body_len = resp_pos;

    return http_status;
}

/* ── Deadline-bounded streaming request ──────────────────────── */

/* Every I/O below is one slice at a time.  The socket timeout is recomputed
 * from the remaining budget before each slice, so a request cannot outlive
 * its deadline even if the peer stops sending, and a cancel is noticed
 * within VELA_REQ_IO_SLICE_MS rather than at the next phase boundary. */

struct vela_emit {
    const vela_resp_sink_t *sink;
    char  *buf;
    size_t cap;
    size_t len;
    bool   truncated;
    bool   drain_sink;
    bool   sink_done;
    uint32_t sink_done_at;
    size_t drained;
};

static int emit_bytes(struct vela_emit *e, const char *data, size_t len)
{
    size_t room;
    size_t copy;

    if (len == 0) {
        return 0;
    }

    if (e->sink != NULL) {
        if (e->sink_done) {
            e->drained += len;
            return e->drained > 65536 ? -1 : 0;
        }
        int rc = e->sink->write(e->sink->ctx, data, len);
        if (rc > 0 && e->drain_sink) {
            e->sink_done = true;
            e->sink_done_at = agent_mono_ms();
            return 0;
        }
        return rc;
    }

    if (e->buf == NULL || e->cap == 0) {
        e->truncated = true;
        return 0;
    }

    room = e->cap - 1 - e->len;
    copy = (len < room) ? len : room;

    if (copy > 0) {
        memcpy(e->buf + e->len, data, copy);
        e->len += copy;
        e->buf[e->len] = '\0';
    }

    if (copy < len) {
        e->truncated = true;
    }

    return 0;
}

/* Incremental chunked decoder.  The whole body is never in one buffer, so
 * vela_http_chunked_complete() cannot be used here: the state has to advance
 * as bytes arrive. A zero-size chunk starts the trailers; the empty line
 * after them is the boundary required before reusing the connection. */

enum vela_chunk_state {
    VELA_CHUNK_SIZE = 0,
    VELA_CHUNK_DATA,
    VELA_CHUNK_DATA_END,
    VELA_CHUNK_TRAILERS,
    VELA_CHUNK_DONE
};

struct vela_chunk_dec {
    enum vela_chunk_state state;
    size_t remaining;
    char   line[24];
    size_t line_len;
};

static int chunk_feed(struct vela_chunk_dec *d, struct vela_emit *e,
    const char *data, size_t len)
{
    size_t i = 0;

    while (i < len && d->state != VELA_CHUNK_DONE) {
        switch (d->state) {
        case VELA_CHUNK_SIZE:
            while (i < len) {
                char c = data[i++];

                if (c == '\n') {
                    unsigned long sz;
                    char *end;

                    d->line[d->line_len] = '\0';
                    errno = 0;
                    sz = strtoul(d->line, &end, 16);
                    if (end == d->line || errno != 0 ||
                        d->line[0] == '-' || d->line[0] == '+' ||
                        (*end != '\0' && *end != ';')) {
                        return -1;
                    }
                    d->line_len = 0;

                    if (sz == 0) {
                        d->state = VELA_CHUNK_TRAILERS;
                    } else {
                        d->remaining = (size_t)sz;
                        d->state = VELA_CHUNK_DATA;
                    }

                    break;
                }

                if (c != '\r') {
                    if (d->line_len + 1 >= sizeof(d->line)) return -1;
                    d->line[d->line_len++] = c;
                }
            }
            break;

        case VELA_CHUNK_DATA:
            {
                size_t take = len - i;

                if (take > d->remaining) {
                    take = d->remaining;
                }

                int emit_rc = emit_bytes(e, data + i, take);
                if (emit_rc < 0) {
                    return -1;
                }
                if (emit_rc > 0) {
                    return 1;
                }

                i += take;
                d->remaining -= take;

                if (d->remaining == 0) {
                    d->state = VELA_CHUNK_DATA_END;
                }
            }
            break;

        case VELA_CHUNK_DATA_END:
            while (i < len) {
                char c = data[i++];

                if (c == '\n') {
                    d->state = VELA_CHUNK_SIZE;
                    break;
                }
            }
            break;

        case VELA_CHUNK_DONE:
            break;

        case VELA_CHUNK_TRAILERS:
            while (i < len) {
                char c = data[i++];
                if (c == '\n') {
                    if (d->line_len == 0) d->state = VELA_CHUNK_DONE;
                    d->line_len = 0;
                    break;
                }
                if (c != '\r' && ++d->line_len > 8192) return -1;
            }
            break;
        }
    }

    /* There is only one outstanding HTTP request on this connection. */
    return i == len ? 0 : -1;
}

/* A read that only means "nothing yet" must not end the request. */

static bool stream_read_retryable(int ret)
{
    return ret == MBEDTLS_ERR_SSL_WANT_READ
        || ret == MBEDTLS_ERR_SSL_WANT_WRITE
        || ret == MBEDTLS_ERR_SSL_TIMEOUT
        || (ret == MBEDTLS_ERR_NET_RECV_FAILED
            && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR));
}

static void stream_set_timeout(tls_ctx_t *ctx, uint32_t ms)
{
    struct timeval tv;

    if (ctx->net.fd < 0) {
        return;
    }

    if (ms == 0) {
        ms = 1;
    }

    tv.tv_sec = (time_t)(ms / 1000u);
    tv.tv_usec = (suseconds_t)((ms % 1000u) * 1000u);

    if (tv.tv_sec == 0 && tv.tv_usec == 0) {
        tv.tv_usec = 1000;
    }

    setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(ctx->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

static int stream_write_all(tls_ctx_t *ctx, const char *what,
    const char *buf, size_t len, const vela_req_ctl_t *ctl)
{
    size_t off = 0;
    uint32_t last_progress = agent_mono_ms();
    int stalls = 0;

    while (off < len) {
        uint32_t now = agent_mono_ms();
        size_t chunk;
        int ret;

        if (vela_req_cancelled(ctl)) {
            return VELA_TLS_ERR_CANCELLED;
        }

        if (vela_req_budget_ms(ctl, now) == 0) {
            return VELA_TLS_ERR_TIMEOUT;
        }

        /* No deadline and no progress: bound the wait anyway, so a stalled
         * peer cannot hold the request thread forever. */

        if (ctl == NULL || ctl->deadline_ms == 0) {
            if (agent_elapsed_ms(last_progress, now) > 60000) {
                syslog(LOG_ERR, "[%s] %s write stalled with no deadline\n",
                    TAG, what);
                return VELA_TLS_ERR_TIMEOUT;
            }
        }

        stream_set_timeout(ctx, vela_req_io_timeout_ms(ctl, now,
            VELA_REQ_IO_SLICE_MS));

        chunk = len - off;
        /* 受控流可使用完整 TLS 记录；原来的普通请求分片保持不变。 */
        size_t limit = ctl != NULL ? 16384u : TLS_WRITE_CHUNK;
        if (chunk > limit) chunk = limit;

        ret = mbedtls_ssl_write(&ctx->ssl, (const unsigned char *)buf + off,
            chunk);

        if (ret > 0) {
            off += (size_t)ret;
            last_progress = agent_mono_ms();
            stalls = 0;
            continue;
        }

        if (ret == 0) {
            return VELA_TLS_ERR_WRITE;
        }

        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE
            || ret == MBEDTLS_ERR_SSL_WANT_READ
            || (ret == MBEDTLS_ERR_NET_SEND_FAILED
                && (errno == EAGAIN || errno == EWOULDBLOCK ||
                    errno == EINTR || errno == ENOMEM))
            || ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            /* A full send buffer is retryable, but only for a bounded
             * number of slices. */

            int wait_rc = tls_wait_socket(ctx->net.fd,
                ret == MBEDTLS_ERR_SSL_WANT_READ ? POLLIN : POLLOUT, ctl);
            if (wait_rc != 0) {
                return wait_rc;
            }

            /* A peer that went away leaves an error on the socket; the
             * next write would only spin until the budget is gone. */

            {
                int soerr = 0;
                socklen_t slen = sizeof(soerr);

                if (getsockopt(ctx->net.fd, SOL_SOCKET, SO_ERROR, &soerr,
                               &slen) == 0 && soerr != 0) {
                    syslog(LOG_ERR,
                        "[%s] %s write sees socket error %d at %u/%u\n",
                        TAG, what, soerr, (unsigned)off, (unsigned)len);
                    return VELA_TLS_ERR_WRITE;
                }
            }

            if (++stalls > 120) {
                syslog(LOG_ERR, "[%s] %s write gave up at %u/%u\n", TAG, what,
                    (unsigned)off, (unsigned)len);
                return VELA_TLS_ERR_WRITE;
            }

            continue;
        }

        syslog(LOG_ERR, "[%s] %s write failed ret=-0x%04x errno=%d at %u/%u\n", TAG,
            what, -ret, errno, (unsigned)off, (unsigned)len);
        return VELA_TLS_ERR_WRITE;
    }

    return 0;
}

/* 有界合并上传源的短片段，避免前缀、WAV 头和每个余片各发一条 TLS 记录。
 * 缓冲放在堆上；不增加语音工作线程栈，也不延长原取消与截止预算。 */
static int stream_send_body(tls_ctx_t *ctx, const vela_body_src_t *body,
                            const vela_req_ctl_t *ctl)
{
    if (body == NULL || body->len == 0) return 0;
    if (body->read == NULL) return VELA_TLS_ERR_READ;
    const size_t capacity = ctl != NULL ? 16384u : 4096u;
    char *piece = malloc(capacity);
    if (!piece) return VELA_TLS_ERR_OVERFLOW;
    size_t sent = 0;
    int ret = 0;
    while (sent < body->len) {
        size_t target = body->len - sent;
        if (target > capacity) target = capacity;
        size_t used = 0;
        while (used < target) {
            if (vela_req_cancelled(ctl)) { ret = VELA_TLS_ERR_CANCELLED; goto out; }
            if (vela_req_budget_ms(ctl, agent_mono_ms()) == 0) {
                ret = VELA_TLS_ERR_TIMEOUT; goto out;
            }
            int got = body->read(body->ctx, piece + used, target - used);
            if (got < 0) { ret = VELA_TLS_ERR_READ; goto out; }
            if (got == 0) { ret = VELA_TLS_ERR_WRITE; goto out; }
            if ((size_t)got > target - used) { ret = VELA_TLS_ERR_OVERFLOW; goto out; }
            used += (size_t)got;
        }
        if (g_req_dump && (sent > 0 || used > 0) &&
            (g_req_dump >= 2 || sent == 0))
          tls_dump_hex("sreqbody", piece,
                       g_req_dump >= 2 ? used : (used < 96 ? used : 96));

        ret = stream_write_all(ctx, "body", piece, used, ctl);
        if (ret != 0) goto out;
        sent += used;
    }
out:
    /* 上传片段可能包含录音或提示词，归还堆之前清零。 */
    for (size_t i=0;i<capacity;i++) ((volatile unsigned char *)piece)[i]=0;
    free(piece);
    return ret;
}

/* http_total_ms must cover the whole transport call, including the early
 * failure exits, so the phases always add up to something that was really
 * spent.  A NULL timing (Feishu, vision, web search) costs one branch. */
static void stream_note_total(agent_llm_timing_t *timing, uint32_t t0)
{
    if (timing != NULL) {
        timing->http_total_ms = agent_elapsed_ms(t0, agent_mono_ms());
    }
}

/* Outcome flags the caller of stream_exchange needs to decide whether
 * the connection may serve another request. */
struct stream_outcome
{
    bool framing_complete;  /* Content-Length satisfied or zero chunk consumed */
    bool sink_early;        /* the sink reported completion before framing ended */
    bool got_response_byte; /* at least one response byte arrived */
    bool response_close;
};

static int stream_exchange(
    tls_ctx_t *ctx,
    const char *host,
    const char *method,
    const char *path,
    const vela_header_t *headers,
    const vela_body_src_t *body,
    const vela_resp_sink_t *sink,
    const vela_req_ctl_t *ctl,
    char *resp_buf,
    size_t resp_cap,
    size_t *out_body_len,
    int *out_status,
    agent_llm_timing_t *timing,
    bool keep_alive,
    struct stream_outcome *outcome,
    uint32_t t_http0);


int vela_https_request_stream(
    const char *host,
    const char *port,
    const char *method,
    const char *path,
    const vela_header_t *headers,
    const vela_body_src_t *body,
    const vela_resp_sink_t *sink,
    const vela_req_ctl_t *ctl,
    char *resp_buf,
    size_t resp_cap,
    size_t *out_body_len,
    int *out_status,
    agent_llm_timing_t *timing)
{
    tls_ctx_t ctx;
    struct stream_outcome outcome;
    uint32_t t_http0 = agent_mono_ms();
    int ret;

    if (host == NULL || port == NULL || method == NULL || path == NULL) {
        stream_note_total(timing, t_http0);
        return VELA_TLS_ERR_CONNECT;
    }

    if (timing != NULL) {
        timing->streamed = true;
    }

    if (out_body_len != NULL) {
        *out_body_len = 0;
    }

    if (out_status != NULL) {
        *out_status = 0;
    }

    if (resp_buf != NULL && resp_cap > 0) {
        resp_buf[0] = '\0';
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.abort_on_close = ctl != NULL;

    if (vela_req_cancelled(ctl)) {
        stream_note_total(timing, t_http0);
        return VELA_TLS_ERR_CANCELLED;
    }

    if (vela_req_budget_ms(ctl, agent_mono_ms()) == 0) {
        stream_note_total(timing, t_http0);
        return VELA_TLS_ERR_TIMEOUT;
    }

    ret = tls_ctx_connect(&ctx, host, port, timing, ctl);
    if (ret != 0) {
        tls_ctx_free(&ctx);
        stream_note_total(timing, t_http0);
        return ret;
    }

    ret = stream_exchange(&ctx, host, method, path, headers, body, sink, ctl,
        resp_buf, resp_cap, out_body_len, out_status, timing, false,
        &outcome, t_http0);

    /* Reclaim before returning, on every path.  A cancelled request must not
     * leave a socket, a TLS context or a partially read record behind: the
     * next request has to be able to start immediately. */

    ctx.abort_on_close = ctl != NULL && ret < 0;
    tls_ctx_free(&ctx);
    return ret;
}

#ifdef CONFIG_AI_AGENT_VOICE_CONN_SLOT

int vela_https_request_stream_voice(
    const char *host,
    const char *port,
    const char *method,
    const char *path,
    const vela_header_t *headers,
    const vela_body_src_t *body,
    const vela_resp_sink_t *sink,
    const vela_req_ctl_t *ctl,
    char *resp_buf,
    size_t resp_cap,
    size_t *out_body_len,
    int *out_status,
    agent_llm_timing_t *timing)
{
    conn_slot_t *slot;
    struct stream_outcome outcome;
    uint32_t t_http0 = agent_mono_ms();
    bool reused = false;
    int ret;

    if (host == NULL || port == NULL || method == NULL || path == NULL) {
        stream_note_total(timing, t_http0);
        return VELA_TLS_ERR_CONNECT;
    }

    if (timing != NULL) {
        timing->streamed = true;
    }

    if (out_body_len != NULL) {
        *out_body_len = 0;
    }

    if (out_status != NULL) {
        *out_status = 0;
    }

    if (resp_buf != NULL && resp_cap > 0) {
        resp_buf[0] = '\0';
    }

    if (vela_req_cancelled(ctl)) {
        stream_note_total(timing, t_http0);
        return VELA_TLS_ERR_CANCELLED;
    }

    if (vela_req_budget_ms(ctl, agent_mono_ms()) == 0) {
        stream_note_total(timing, t_http0);
        return VELA_TLS_ERR_TIMEOUT;
    }

    slot = voice_slot_try_acquire(host, port);
    if (slot == NULL) {
        /* Busy — a preconnect is still filling it.  A plain ephemeral
         * request keeps the round moving; the slot serves the next one. */

        return vela_https_request_stream(host, port, method, path, headers,
            body, sink, ctl, resp_buf, resp_cap, out_body_len, out_status,
            timing);
    }

    if (slot->valid && !voice_slot_alive(&slot->ctx)) {
        syslog(LOG_INFO, "[%s] voice_conn stale, reconnecting\n", TAG);
        tls_ctx_free(&slot->ctx);
        slot->valid = false;
    }

    if (slot->valid) {
        reused = true;
    } else {
        memset(&slot->ctx, 0, sizeof(slot->ctx));
        slot->ctx.abort_on_close = ctl != NULL;

        ret = tls_ctx_connect(&slot->ctx, host, port, timing, ctl);
        if (ret != 0) {
            tls_ctx_free(&slot->ctx);
            voice_slot_release(host, port, false);
            stream_note_total(timing, t_http0);
            return ret;
        }
    }

    if (timing != NULL) {
        timing->reused = reused;
        if (reused) {
            /* No handshake ran: the phases stay NA so the log shows what
             * actually executed, and reused=1 marks the hit. */

            timing->dns_tcp_ms = AGENT_T_NA;
            timing->tls_setup_ms = AGENT_T_NA;
            timing->tls_ms = AGENT_T_NA;
        }
    }

    ret = stream_exchange(&slot->ctx, host, method, path, headers, body, sink,
        ctl, resp_buf, resp_cap, out_body_len, out_status, timing, true,
        &outcome, t_http0);

    if (voice_slot_keep(ret, outcome.framing_complete, outcome.response_close)) {
        voice_slot_release(host, port, true);
        return ret;
    }

    /* Everything else destroys the connection; an aborted request
     * resets (RST) exactly like the plain entry. */

    syslog(LOG_INFO, "[%s] voice_conn drop rc=%d framing=%d sink_early=%d\n",
           TAG, ret, outcome.framing_complete ? 1 : 0,
           outcome.sink_early ? 1 : 0);
    slot->ctx.abort_on_close = ctl != NULL && ret < 0;
    tls_ctx_free(&slot->ctx);
    slot->valid = false;
    voice_slot_release(host, port, false);

    /* After exchange starts, a POST may already have reached the server
     * and a streaming upload source may be exhausted. Do not silently
     * replay it. Stale sockets detected before exchange reconnect above;
     * an ambiguous failure is reported and the next request starts fresh. */

    return ret;
}

int vela_https_voice_preconnect(const char *host, const char *port,
                                const vela_req_ctl_t *ctl,
                                agent_llm_timing_t *timing)
{
    conn_slot_t *slot;
    uint32_t t0;

    if (host == NULL || port == NULL) {
        return VELA_TLS_ERR_CONNECT;
    }

    slot = voice_slot_try_acquire(host, port);
    if (slot == NULL) {
        return 0;   /* already warming, or a request owns it */
    }

    if (slot->valid) {
        if (voice_slot_alive(&slot->ctx)) {
            voice_slot_release(host, port, true);
            return 0;   /* already warm */
        }

        tls_ctx_free(&slot->ctx);
        slot->valid = false;
    }

    t0 = agent_mono_ms();
    memset(&slot->ctx, 0, sizeof(slot->ctx));

    if (tls_ctx_connect(&slot->ctx, host, port, timing, ctl) != 0) {
        tls_ctx_free(&slot->ctx);
        voice_slot_release(host, port, false);
        return VELA_TLS_ERR_CONNECT;
    }

    /* Release before the log line: a request arriving between the print
     * and the release would find the slot busy and pay a connection it
     * should have reused. */

    voice_slot_release(host, port, true);
    syslog(LOG_INFO, "[%s] voice_preconnect %s:%s ms=%u\n", TAG, host, port,
           (unsigned)(agent_mono_ms() - t0));
    return 0;
}

#else /* !CONFIG_AI_AGENT_VOICE_CONN_SLOT */

int vela_https_request_stream_voice(
    const char *host,
    const char *port,
    const char *method,
    const char *path,
    const vela_header_t *headers,
    const vela_body_src_t *body,
    const vela_resp_sink_t *sink,
    const vela_req_ctl_t *ctl,
    char *resp_buf,
    size_t resp_cap,
    size_t *out_body_len,
    int *out_status,
    agent_llm_timing_t *timing)
{
    return vela_https_request_stream(host, port, method, path, headers, body,
        sink, ctl, resp_buf, resp_cap, out_body_len, out_status, timing);
}

int vela_https_voice_preconnect(const char *host, const char *port,
                                const vela_req_ctl_t *ctl,
                                agent_llm_timing_t *timing)
{
    (void)host;
    (void)port;
    (void)ctl;
    (void)timing;
    return 0;
}

#endif /* CONFIG_AI_AGENT_VOICE_CONN_SLOT */

/* One HTTP request/response over an already connected ctx.  The ctx is
 * owned by the caller and never freed here; on return the caller closes
 * it or, when keep_alive was requested and the outcome allows, keeps it
 * for the next exchange.  Header build, send with per-slice timeouts,
 * response parsing and framing detection are the original stream path
 * unchanged. */
static int stream_exchange(
    tls_ctx_t *ctx,
    const char *host,
    const char *method,
    const char *path,
    const vela_header_t *headers,
    const vela_body_src_t *body,
    const vela_resp_sink_t *sink,
    const vela_req_ctl_t *ctl,
    char *resp_buf,
    size_t resp_cap,
    size_t *out_body_len,
    int *out_status,
    agent_llm_timing_t *timing,
    bool keep_alive,
    struct stream_outcome *outcome,
    uint32_t t_http0)
{
    struct vela_emit emit = {0};
    struct vela_chunk_dec chunk;
    char *hdr = NULL;
    char tmp[8192];
    size_t hdr_len = 0;
    size_t body_got = 0;
    long content_length = -1;
    bool chunked = false;
    bool header_done = false;
    bool eof = false;
    bool complete = false;
    uint32_t mark;
    uint32_t last_progress;
    uint32_t t_first_byte = 0;
    bool ttfb_seen = false;
    int status = 0;
    int ret = 0;
    int n;

    memset(&chunk, 0, sizeof(chunk));
    memset(outcome, 0, sizeof(*outcome));

    emit.sink = sink;
    emit.buf = resp_buf;
    emit.cap = resp_cap;
    emit.len = 0;
    emit.truncated = false;
    emit.drain_sink = keep_alive;

    last_progress = agent_mono_ms();

    mark = agent_mono_ms();

    /* Request headers.  Content-Length comes from the source's declared
     * length, so the body is written without ever being assembled. */

    hdr = malloc(4096);
    if (hdr == NULL) {
        ret = VELA_TLS_ERR_OVERFLOW;
        goto out;
    }

    {
        int pos = 0;

#define SHDR(fmt, ...)                                          \
    do {                                                        \
        pos += snprintf(hdr + pos, 4096 - pos, fmt, ##__VA_ARGS__); \
        if (pos >= 4096) {                                      \
            ret = VELA_TLS_ERR_OVERFLOW;                        \
            goto out;                                           \
        }                                                       \
    } while (0)

        SHDR("%s %s HTTP/1.1\r\n", method, path);
        SHDR("Host: %s\r\n", host);
        SHDR("Connection: %s\r\n", keep_alive ? "keep-alive" : "close");
        SHDR("User-Agent: agent-vela/1.0\r\n");

        if (body != NULL && body->len > 0) {
            SHDR("Content-Length: %zu\r\n", body->len);
        }

        if (headers != NULL) {
            for (const vela_header_t *h = headers; h->name != NULL; h++) {
                SHDR("%s: %s\r\n", h->name, h->value);
            }
        }

        SHDR("\r\n");
#undef SHDR

        if (g_req_dump)
          tls_dump_head("sreq", hdr, (size_t)pos);

        ret = stream_write_all(ctx, "hdr", hdr, (size_t)pos, ctl);
    }

    free(hdr);
    hdr = NULL;

    if (ret != 0) {
        /* A failed send is still a measured send: the time already spent
         * belongs in send_ms, while ttfb and body stay NA. */
        if (timing != NULL) {
            timing->send_ms = agent_lap_ms(&mark);
        }
        goto out;
    }

    ret = stream_send_body(ctx, body, ctl);
    if (ret != 0) {
        if (timing != NULL) {
            timing->send_ms = agent_lap_ms(&mark);
        }
        goto out;
    }

    /* send covers headers and body, the same contract as the buffered
     * request path, so the phases stay comparable across both entries.
     * After the lap, mark is the send-done instant the sink layer anchors
     * its own first-data and block timestamps to. */
    if (timing != NULL) {
        timing->send_ms = agent_lap_ms(&mark);
        timing->send_done_mono = mark;
    }

    /* Response headers. */

    while (!header_done && hdr_len < sizeof(tmp) - 1) {
        uint32_t now = agent_mono_ms();

        if (vela_req_cancelled(ctl)) {
            ret = VELA_TLS_ERR_CANCELLED;
            goto out;
        }

        if (vela_req_budget_ms(ctl, now) == 0) {
            ret = VELA_TLS_ERR_TIMEOUT;
            goto out;
        }

        if ((ctl == NULL || ctl->deadline_ms == 0)
            && agent_elapsed_ms(last_progress, now) > 60000) {
            ret = VELA_TLS_ERR_TIMEOUT;
            goto out;
        }

        stream_set_timeout(ctx, vela_req_io_timeout_ms(ctl, now,
            VELA_REQ_IO_SLICE_MS));

        n = mbedtls_ssl_read(&ctx->ssl, (unsigned char *)tmp + hdr_len,
            sizeof(tmp) - 1 - hdr_len);

        if (n > 0) {
            if (!ttfb_seen) {
                /* The first byte is the status line, not a model token;
                 * the sink layer records the first SSE data line
                 * separately.  Kept on later error paths: a reply that
                 * arrived and then broke has still paid its ttfb. */
                ttfb_seen = true;
                outcome->got_response_byte = true;
                t_first_byte = agent_mono_ms();
                if (timing != NULL) {
                    timing->ttfb_ms = agent_elapsed_ms(mark, t_first_byte);
                }
            }
            hdr_len += (size_t)n;
            tmp[hdr_len] = '\0';
            last_progress = agent_mono_ms();

            if (memmem(tmp, hdr_len, "\r\n\r\n", 4) != NULL) {
                header_done = true;
            }

            continue;
        }

        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            eof = true;
            break;
        }

        if (stream_read_retryable(n)) {
            ret = tls_wait_socket(ctx->net.fd,
                n == MBEDTLS_ERR_SSL_WANT_WRITE ? POLLOUT : POLLIN, ctl);
            if (ret != 0) {
                goto out;
            }
            continue;
        }

        syslog(LOG_ERR, "[%s] stream header read ret=-0x%04x\n", TAG, -n);
        ret = VELA_TLS_ERR_READ;
        goto out;
    }

    if (!header_done) {
        ret = eof ? VELA_TLS_ERR_READ : VELA_TLS_ERR_OVERFLOW;
        goto out;
    }

    if (sscanf(tmp, "HTTP/1.%*d %d", &status) != 1) {
        syslog(LOG_ERR, "[%s] stream: bad status line: %.80s\n", TAG, tmp);
        ret = VELA_TLS_ERR_READ;
        goto out;
    }

    if (out_status != NULL) *out_status = status;

    if (status != 200) {
        /* The raw response head says who rejected the request and why;
         * it carries no credentials.  Truncated on purpose. */
        syslog(LOG_ERR, "[%s] stream: status=%d head: %.256s\n", TAG,
               status, tmp);
    }

    {
        char *body_start = (char *)memmem(tmp, hdr_len, "\r\n\r\n", 4) + 4;
        char *cl;
        char *te;
        size_t initial = hdr_len - (size_t)(body_start - tmp);

        char *connection = strcasestr(tmp, "\r\nConnection:");
        if (connection != NULL && connection < body_start) {
            char *line_end = strstr(connection + 2, "\r\n");
            char *close_token = strcasestr(connection, "close");
            outcome->response_close = close_token != NULL &&
                                      close_token < line_end;
        }
        if (strncmp(tmp, "HTTP/1.0 ", 9) == 0) {
            outcome->response_close = true;
        }

        cl = strcasestr(tmp, "Content-Length:");
        if (cl != NULL && cl < body_start) {
            content_length = strtol(cl + strlen("Content-Length:"), NULL, 10);
            if (content_length < 0 || content_length > 16 * 1024 * 1024) {
                content_length = -1;
            }
        }

        te = strcasestr(tmp, "Transfer-Encoding:");
        if (te != NULL && te < body_start) {
            chunked = (strcasestr(te, "chunked") != NULL);
        }

        if (initial > 0) {
            if (chunked) {
                int cf_rc = chunk_feed(&chunk, &emit, body_start, initial);
                if (cf_rc < 0) {
                    ret = VELA_TLS_ERR_SINK;
                    goto out;
                }
                if (cf_rc > 0) {
                    complete = true;
                    outcome->sink_early = true;
                }
                if (chunk.state == VELA_CHUNK_DONE) {
                    complete = true;
                    outcome->framing_complete = true;
                }
            } else {
                size_t take = initial;

                if (content_length >= 0 && (long)take > content_length) {
                    take = (size_t)content_length;
                }

                int emit_rc = emit_bytes(&emit, body_start, take);
                if (emit_rc < 0) {
                    ret = VELA_TLS_ERR_SINK;
                    goto out;
                }
                if (emit_rc > 0) {
                    complete = true;
                    outcome->sink_early = true;
                }

                body_got = take;
                if (content_length >= 0 && body_got >= (size_t)content_length) {
                    complete = true;
                    outcome->framing_complete = true;
                }
            }
        }
    }

    /* Response body. */

    for (;;) {
        uint32_t now;
        vela_req_ctl_t drain_ctl;
        const vela_req_ctl_t *read_ctl = ctl;

        if (complete) {
            break;
        }

        if (chunked) {
            complete = (chunk.state == VELA_CHUNK_DONE);
            if (complete) {
                outcome->framing_complete = true;
            }
        } else if (content_length >= 0) {
            complete = (body_got >= (size_t)content_length);
            if (complete) {
                outcome->framing_complete = true;
            }
        } else {
            complete = eof;
        }

        if (complete) {
            break;
        }

        now = agent_mono_ms();

        if (emit.sink_done &&
            agent_elapsed_ms(emit.sink_done_at, now) >= 2000) {
            ret = VELA_TLS_ERR_TIMEOUT;
            goto out;
        }
        if (emit.sink_done) {
            drain_ctl.cancel = ctl != NULL ? ctl->cancel : NULL;
            drain_ctl.deadline_ms = now + 2000 -
                (uint32_t)agent_elapsed_ms(emit.sink_done_at, now);
            if (drain_ctl.deadline_ms == 0) drain_ctl.deadline_ms = 1;
            int32_t left = vela_req_budget_ms(ctl, now);
            if (left < 0 || left >
                vela_req_budget_ms(&drain_ctl, now)) {
                read_ctl = &drain_ctl;
            }
        }

        if (vela_req_cancelled(ctl)) {
            ret = VELA_TLS_ERR_CANCELLED;
            goto out;
        }

        if (vela_req_budget_ms(ctl, now) == 0) {
            ret = VELA_TLS_ERR_TIMEOUT;
            goto out;
        }

        if ((ctl == NULL || ctl->deadline_ms == 0)
            && agent_elapsed_ms(last_progress, now) > 60000) {
            ret = VELA_TLS_ERR_TIMEOUT;
            goto out;
        }

        stream_set_timeout(ctx, vela_req_io_timeout_ms(read_ctl, now,
            VELA_REQ_IO_SLICE_MS));

        n = mbedtls_ssl_read(&ctx->ssl, (unsigned char *)tmp, sizeof(tmp));

        if (n > 0) {
            last_progress = agent_mono_ms();

            if (chunked) {
                int cf_rc = chunk_feed(&chunk, &emit, tmp, (size_t)n);
                if (cf_rc < 0) {
                    ret = VELA_TLS_ERR_SINK;
                    goto out;
                }
                if (cf_rc > 0) {
                    outcome->sink_early = true;
                    break;
                }
                if (chunk.state == VELA_CHUNK_DONE) {
                    outcome->framing_complete = true;
                    break;
                }
            } else {
                size_t take = (size_t)n;

                if (content_length >= 0
                    && body_got + take > (size_t)content_length) {
                    take = (size_t)content_length - body_got;
                }

                int emit_rc = emit_bytes(&emit, tmp, take);
                if (emit_rc < 0) {
                    ret = VELA_TLS_ERR_SINK;
                    goto out;
                }
                if (emit_rc > 0) {
                    outcome->sink_early = true;
                    break;
                }

                body_got += take;
                if (content_length >= 0
                    && body_got >= (size_t)content_length) {
                    outcome->framing_complete = true;
                    break;
                }
            }

            continue;
        }

        if (n == 0 || n == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            eof = true;

            /* An unframed body ends at EOF; a framed one that ends early is
             * truncated and must not be reported as complete. */

            if (chunked && chunk.state != VELA_CHUNK_DONE) {
                syslog(LOG_ERR, "[%s] chunked body ended before zero chunk\n",
                    TAG);
                ret = VELA_TLS_ERR_READ;
                goto out;
            }

            if (content_length >= 0 && body_got < (size_t)content_length) {
                syslog(LOG_ERR, "[%s] body ended at %u/%ld\n", TAG,
                    (unsigned)body_got, content_length);
                ret = VELA_TLS_ERR_READ;
                goto out;
            }

            break;
        }

        if (stream_read_retryable(n)) {
            ret = tls_wait_socket(ctx->net.fd,
                n == MBEDTLS_ERR_SSL_WANT_WRITE ? POLLOUT : POLLIN, read_ctl);
            if (ret != 0) {
                goto out;
            }
            continue;
        }

        syslog(LOG_ERR, "[%s] stream body read ret=-0x%04x\n", TAG, -n);
        ret = VELA_TLS_ERR_READ;
        goto out;
    }

    if (timing != NULL && ttfb_seen) {
        /* Measured from the first byte so headers and body form one span
         * that never overlaps ttfb_ms, the same convention as the buffered
         * reader.  Only the success path reaches here; an incomplete body
         * keeps NA instead of a partial span. */
        timing->body_ms = agent_elapsed_ms(t_first_byte, agent_mono_ms());
    }

    if (emit.truncated) {
        syslog(LOG_ERR, "[%s] streamed response did not fit the %zu byte "
            "buffer; caller must treat it as incomplete\n", TAG, resp_cap);
    }

    if (out_body_len != NULL) {
        *out_body_len = (sink != NULL) ? body_got : emit.len;
    }

    if (out_status != NULL) {
        *out_status = status;
    }

out:
    free(hdr);
    if (emit.sink_done) outcome->sink_early = true;
    stream_note_total(timing, t_http0);
    return ret;
}

/* ── Public API ──────────────────────────────────────────────── */

int vela_https_request_timed(
    const char* host,
    const char* port,
    const char* method,
    const char* path,
    const vela_header_t* headers,
    const char* body,
    size_t body_len,
    char* resp_buf,
    size_t resp_cap,
    size_t* out_body_len,
    agent_llm_timing_t* timing)
{
    int ret;
    uint32_t mark = agent_mono_ms();

    /* Try to get a pooled connection first */
    conn_slot_t* slot = pool_acquire(host, port);

    if (timing != NULL) {
        /* Pool time covers waiting for the global pool lock as well as the
         * slot scan, so it is the real price of serialized requests. */

        timing->pool_ms = agent_lap_ms(&mark);
    }

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
        if (timing != NULL) {
            timing->reused = true;
        }

        mark = agent_mono_ms();
        ret = tls_write_request(&slot->ctx, method, host, path, headers, body, body_len);
        if (timing != NULL) {
            timing->send_ms = agent_lap_ms(&mark);
            timing->send_done_mono = mark;
        }

        if (ret == 0) {
            bool keep = false;
            ret = tls_read_response(&slot->ctx, resp_buf, resp_cap, out_body_len,
                &keep, mark, timing);
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
        if (timing != NULL) {
            /* The reuse did not happen; the retry below pays for a full
             * connect, and the breakdown has to say so. */

            timing->reused = false;
        }
    }

    /* New connection */
    if (!slot) {
        /* Pool full — use a temporary ephemeral context */
        tls_ctx_t tmp_ctx;
        if ((ret = tls_ctx_connect(&tmp_ctx, host, port, timing, NULL)) != 0) {
            tls_ctx_free(&tmp_ctx);
            return ret;
        }
        mark = agent_mono_ms();
        if ((ret = tls_write_request(&tmp_ctx, method, host, path,
                 headers, body, body_len))
            != 0) {
            if (timing != NULL) {
                timing->send_ms = agent_lap_ms(&mark);
            }
            syslog(LOG_ERR, "[%s] Write request failed: %d\n", TAG, ret);
            tls_ctx_free(&tmp_ctx);
            return ret;
        }
        if (timing != NULL) {
            timing->send_ms = agent_lap_ms(&mark);
            timing->send_done_mono = mark;
        }
        ret = tls_read_response(&tmp_ctx, resp_buf, resp_cap, out_body_len,
            NULL, mark, timing);
        tls_ctx_free(&tmp_ctx);
        return ret;
    }

    /* Connect into the slot */
    if ((ret = tls_ctx_connect(&slot->ctx, host, port, timing, NULL)) != 0) {
        pool_release(slot, host, port, false);
        return ret;
    }

    mark = agent_mono_ms();
    if ((ret = tls_write_request(&slot->ctx, method, host, path,
             headers, body, body_len))
        != 0) {
        if (timing != NULL) {
            timing->send_ms = agent_lap_ms(&mark);
        }
        syslog(LOG_ERR, "[%s] Write request failed: %d\n", TAG, ret);
        pool_release(slot, host, port, false);
        return ret;
    }
    if (timing != NULL) {
        timing->send_ms = agent_lap_ms(&mark);
        timing->send_done_mono = mark;
    }

    bool keep = false;
    ret = tls_read_response(&slot->ctx, resp_buf, resp_cap, out_body_len,
        &keep, mark, timing);
    pool_release(slot, host, port, ret > 0 && keep);
    return ret;
}

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
    return vela_https_request_timed(host, port, method, path, headers, body,
        body_len, resp_buf, resp_cap, out_body_len, NULL);
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

int vela_https_head_date(const char* host, const char* port, const char* path,
    char* date_out, size_t date_cap)
{
    tls_ctx_t ctx;
    int ret;

    if ((ret = tls_ctx_connect(&ctx, host, port, NULL, NULL)) != 0) {
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

    /* Keep reading body, with the same framing rules as the TLS path. */

    while (!eof && resp_pos < resp_cap - 1) {
        if (vela_http_body_complete(chunked, content_length, resp_pos,
                resp_buf)) {
            break;
        }

        ssize_t n = read(fd, resp_buf + resp_pos, resp_cap - 1 - resp_pos);
        if (n <= 0) {
            break;
        }

        resp_pos += (size_t)n;
    }

    resp_buf[resp_pos] = '\0';
    free(raw);
    close(fd);

    /* Same as the TLS path: a full buffer means the body was cut off, and the
     * caller would otherwise diagnose it as malformed JSON. */
    if (resp_pos >= resp_cap - 1) {
        syslog(LOG_ERR, "http: response body truncated at %zu bytes "
               "(buf cap %zu): reply larger than buffer\n", resp_pos, resp_cap);
    }

    /* Chunked decode */
    if (chunked) {
        resp_pos = decode_chunked(resp_buf, resp_pos);
        resp_buf[resp_pos] = '\0';
    }

    return http_status;
}
