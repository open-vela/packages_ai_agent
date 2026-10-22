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

/* Timing is optional so the other callers (Feishu, vision, web search) keep
 * their current behaviour.  The marks are taken back to back, so
 * dns_tcp + tls_setup + tls add up to the whole connect. */

static int tls_ctx_connect(tls_ctx_t* ctx, const char* host, const char* port,
    agent_llm_timing_t* timing)
{
    int ret;
    uint32_t mark = agent_mono_ms();

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
        ret = mbedtls_net_connect(&ctx->net, host, port,
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
            return VELA_TLS_ERR_CONNECT;
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
    mbedtls_net_set_block(&ctx->net);
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

    /* Use blocking recv — no select()/poll().  The read
     * timeout is enforced by the SO_RCVTIMEO socket option set above. */
    mbedtls_ssl_set_bio(&ctx->ssl, &ctx->net,
        mbedtls_net_send, mbedtls_net_recv, NULL);

    /* Timer for the handshake starts here, after configuration.  Setup cost
     * is already accounted for in tls_setup_ms so the two do not overlap. */

    mark = agent_mono_ms();

    /* Handshake */
    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
#if defined(MBEDTLS_ERROR_C)
            char err_buf[128];
            mbedtls_strerror(ret, err_buf, sizeof(err_buf));
            syslog(LOG_ERR, "[%s] ssl_handshake ret=-0x%04x: %s\n", TAG, -ret, err_buf);
#else
            syslog(LOG_ERR, "[%s] ssl_handshake ret=-0x%04x\n", TAG, -ret);
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
            return VELA_TLS_ERR_HANDSHAKE;
        }
    }

    if (timing != NULL) {
        timing->tls_ms = agent_lap_ms(&mark);
    }

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
        if (attempts == 1 || attempts >= TLS_WRITE_ATTEMPTS) {
            int err = errno;
            int fl = (ctx->net.fd >= 0) ? fcntl(ctx->net.fd, F_GETFL) : -1;

            syslog(attempts == 1 ? LOG_WARNING : LOG_ERR,
                "[%s] %s write retry %d at %u/%u ret=-0x%04x errno=%d fl=0x%x\n",
                TAG, what, attempts, (unsigned)off, (unsigned)len,
                -ret, err, (unsigned)fl);
        }

        if (attempts >= TLS_WRITE_ATTEMPTS) {
            return VELA_TLS_ERR_WRITE;
        }

        usleep(TLS_WRITE_BACKOFF_US);
    }

    return 0;
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
};

static int emit_bytes(struct vela_emit *e, const char *data, size_t len)
{
    size_t room;
    size_t copy;

    if (len == 0) {
        return 0;
    }

    if (e->sink != NULL) {
        return e->sink->write(e->sink->ctx, data, len) < 0 ? -1 : 0;
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
 * as bytes arrive.  A zero-size chunk ends the message; trailer fields after
 * it are not consumed, which matches the framing rule used by the buffered
 * reader. */

enum vela_chunk_state {
    VELA_CHUNK_SIZE = 0,
    VELA_CHUNK_DATA,
    VELA_CHUNK_DATA_END,
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
                    long sz;

                    d->line[d->line_len] = '\0';
                    sz = strtol(d->line, NULL, 16);
                    d->line_len = 0;

                    if (sz <= 0) {
                        d->state = VELA_CHUNK_DONE;
                    } else {
                        d->remaining = (size_t)sz;
                        d->state = VELA_CHUNK_DATA;
                    }

                    break;
                }

                if (c != '\r' && d->line_len + 1 < sizeof(d->line)) {
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

                if (emit_bytes(e, data + i, take) < 0) {
                    return -1;
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
        }
    }

    return 0;
}

/* A read that only means "nothing yet" must not end the request. */

static bool stream_read_retryable(int ret)
{
    return ret == MBEDTLS_ERR_SSL_WANT_READ
        || ret == MBEDTLS_ERR_SSL_WANT_WRITE
        || ret == MBEDTLS_ERR_SSL_TIMEOUT
        || ret == MBEDTLS_ERR_NET_RECV_FAILED;
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
        if (chunk > TLS_WRITE_CHUNK) {
            chunk = TLS_WRITE_CHUNK;
        }

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
            || ret == MBEDTLS_ERR_NET_SEND_FAILED
            || ret == MBEDTLS_ERR_SSL_TIMEOUT) {
            /* A full send buffer is retryable, but only for a bounded
             * number of slices. */

            if (++stalls > 120) {
                syslog(LOG_ERR, "[%s] %s write gave up at %u/%u\n", TAG, what,
                    (unsigned)off, (unsigned)len);
                return VELA_TLS_ERR_WRITE;
            }

            continue;
        }

        syslog(LOG_ERR, "[%s] %s write failed ret=-0x%04x at %u/%u\n", TAG,
            what, -ret, (unsigned)off, (unsigned)len);
        return VELA_TLS_ERR_WRITE;
    }

    return 0;
}

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
    struct vela_emit emit;
    struct vela_chunk_dec chunk;
    char *hdr = NULL;
    char tmp[2048];
    size_t hdr_len = 0;
    size_t body_got = 0;
    long content_length = -1;
    bool chunked = false;
    bool header_done = false;
    bool eof = false;
    uint32_t mark;
    uint32_t last_progress;
    int status = 0;
    int ret = 0;
    int n;

    if (host == NULL || port == NULL || method == NULL || path == NULL) {
        return VELA_TLS_ERR_CONNECT;
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
    memset(&chunk, 0, sizeof(chunk));

    emit.sink = sink;
    emit.buf = resp_buf;
    emit.cap = resp_cap;
    emit.len = 0;
    emit.truncated = false;

    if (vela_req_cancelled(ctl)) {
        return VELA_TLS_ERR_CANCELLED;
    }

    if (vela_req_budget_ms(ctl, agent_mono_ms()) == 0) {
        return VELA_TLS_ERR_TIMEOUT;
    }

    mark = agent_mono_ms();
    last_progress = mark;

    ret = tls_ctx_connect(&ctx, host, port, timing);
    if (ret != 0) {
        tls_ctx_free(&ctx);
        return ret;
    }

    /* Request headers.  Content-Length comes from the source's declared
     * length, so the body is written without ever being assembled. */

    hdr = malloc(4096);
    if (hdr == NULL) {
        tls_ctx_free(&ctx);
        return VELA_TLS_ERR_OVERFLOW;
    }

    {
        int pos = 0;

#define SHDR(fmt, ...)                                          \
    do {                                                        \
        pos += snprintf(hdr + pos, 4096 - pos, fmt, ##__VA_ARGS__); \
        if (pos >= 4096) {                                      \
            free(hdr);                                          \
            tls_ctx_free(&ctx);                                 \
            return VELA_TLS_ERR_OVERFLOW;                       \
        }                                                       \
    } while (0)

        SHDR("%s %s HTTP/1.1\r\n", method, path);
        SHDR("Host: %s\r\n", host);
        SHDR("Connection: close\r\n");
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

        ret = stream_write_all(&ctx, "hdr", hdr, (size_t)pos, ctl);
    }

    free(hdr);
    hdr = NULL;

    if (ret != 0) {
        tls_ctx_free(&ctx);
        return ret;
    }

    if (timing != NULL) {
        timing->send_ms = agent_elapsed_ms(mark, agent_mono_ms());
    }

    /* Body, one bounded piece at a time. */

    if (body != NULL && body->len > 0) {
        size_t sent = 0;
        char piece[4096];

        while (sent < body->len) {
            int got = body->read(body->ctx, piece, sizeof(piece));

            if (got < 0) {
                tls_ctx_free(&ctx);
                return VELA_TLS_ERR_READ;
            }

            if (got == 0) {
                /* The source promised len bytes and stopped early.  Sending
                 * a short body would leave the server waiting for the rest,
                 * so fail now and name the size. */

                syslog(LOG_ERR, "[%s] body source ended at %u/%u\n", TAG,
                    (unsigned)sent, (unsigned)body->len);
                tls_ctx_free(&ctx);
                return VELA_TLS_ERR_WRITE;
            }

            if (sent + (size_t)got > body->len) {
                got = (int)(body->len - sent);
            }

            ret = stream_write_all(&ctx, "body", piece, (size_t)got, ctl);
            if (ret != 0) {
                tls_ctx_free(&ctx);
                return ret;
            }

            sent += (size_t)got;
        }
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

        stream_set_timeout(&ctx, vela_req_io_timeout_ms(ctl, now,
            VELA_REQ_IO_SLICE_MS));

        n = mbedtls_ssl_read(&ctx.ssl, (unsigned char *)tmp + hdr_len,
            sizeof(tmp) - 1 - hdr_len);

        if (n > 0) {
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

    {
        char *body_start = (char *)memmem(tmp, hdr_len, "\r\n\r\n", 4) + 4;
        char *cl;
        char *te;
        size_t initial = hdr_len - (size_t)(body_start - tmp);

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
                if (chunk_feed(&chunk, &emit, body_start, initial) < 0) {
                    ret = VELA_TLS_ERR_SINK;
                    goto out;
                }
            } else {
                size_t take = initial;

                if (content_length >= 0 && (long)take > content_length) {
                    take = (size_t)content_length;
                }

                if (emit_bytes(&emit, body_start, take) < 0) {
                    ret = VELA_TLS_ERR_SINK;
                    goto out;
                }

                body_got = take;
            }
        }
    }

    /* Response body. */

    for (;;) {
        uint32_t now;
        bool complete;

        if (chunked) {
            complete = (chunk.state == VELA_CHUNK_DONE);
        } else if (content_length >= 0) {
            complete = (body_got >= (size_t)content_length);
        } else {
            complete = eof;
        }

        if (complete) {
            break;
        }

        now = agent_mono_ms();

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

        stream_set_timeout(&ctx, vela_req_io_timeout_ms(ctl, now,
            VELA_REQ_IO_SLICE_MS));

        n = mbedtls_ssl_read(&ctx.ssl, (unsigned char *)tmp, sizeof(tmp));

        if (n > 0) {
            last_progress = agent_mono_ms();

            if (chunked) {
                if (chunk_feed(&chunk, &emit, tmp, (size_t)n) < 0) {
                    ret = VELA_TLS_ERR_SINK;
                    goto out;
                }
            } else {
                size_t take = (size_t)n;

                if (content_length >= 0
                    && body_got + take > (size_t)content_length) {
                    take = (size_t)content_length - body_got;
                }

                if (emit_bytes(&emit, tmp, take) < 0) {
                    ret = VELA_TLS_ERR_SINK;
                    goto out;
                }

                body_got += take;
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
            continue;
        }

        syslog(LOG_ERR, "[%s] stream body read ret=-0x%04x\n", TAG, -n);
        ret = VELA_TLS_ERR_READ;
        goto out;
    }

    if (timing != NULL) {
        timing->ttfb_ms = AGENT_T_NA;
        timing->body_ms = agent_elapsed_ms(mark, agent_mono_ms());
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
    /* Reclaim before returning, on every path.  A cancelled request must not
     * leave a socket, a TLS context or a partially read record behind: the
     * next request has to be able to start immediately. */

    if (ctx.net.fd >= 0) {
        shutdown(ctx.net.fd, SHUT_RDWR);
    }

    tls_ctx_free(&ctx);
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
        if ((ret = tls_ctx_connect(&tmp_ctx, host, port, timing)) != 0) {
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
        }
        ret = tls_read_response(&tmp_ctx, resp_buf, resp_cap, out_body_len,
            NULL, mark, timing);
        tls_ctx_free(&tmp_ctx);
        return ret;
    }

    /* Connect into the slot */
    if ((ret = tls_ctx_connect(&slot->ctx, host, port, timing)) != 0) {
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

    if ((ret = tls_ctx_connect(&ctx, host, port, NULL)) != 0) {
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
