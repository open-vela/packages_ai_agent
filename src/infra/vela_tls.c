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

/* Check whether the raw accumulated body ends with the chunked
 * terminator ("...\r\n0\r\n\r\n", or "0\r\n\r\n" for an empty body).
 * Without this check the read loop below blocks on keep-alive
 * connections until the server closes them (~60s idle timeout),
 * tripping the agent's 60s LLM watchdog. */
static int chunked_end_seen(const char* buf, size_t len)
{
    static const char term_empty[] = "0\r\n\r\n";
    static const char term_last[] = "\r\n0\r\n\r\n";
    size_t n1 = sizeof(term_empty) - 1; /* 5 */
    size_t n2 = sizeof(term_last) - 1;  /* 7 */

    if (len >= n2 && memcmp(buf + len - n2, term_last, n2) == 0)
        return 1;
    if (len >= n1 && memcmp(buf + len - n1, term_empty, n1) == 0)
        return 1;
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
    int reuses;  /* v113b：本连接已被复用的次数（超过上限强制重连） */
    int64_t last_used_ms;
} conn_slot_t;

/* v113b：单调毫秒，用于连接空闲超时判定 */
static int64_t tls_mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static conn_slot_t s_pool[CONN_POOL_SIZE];
static pthread_mutex_t s_pool_lock = PTHREAD_MUTEX_INITIALIZER;

/* v125：上行断开标志位（见 vela_tls_pool_invalidate）。只做一次原子写，
 * 真正的 mbedtls 释放留到下一次 pool_acquire()，避免在 net_poll 这种
 * 小栈线程里跑 mbedtls_ssl_close_notify()/free()。 */
static volatile bool s_pool_stale;

/* ── P0-NET（2026-11-04）：连接/握手分段计时 + 保守预连接 ──
 * 目标：让"连接暖"(TCP+TLS 已完成)在空闲时预先发生，弱网首包少付 1–3s；
 * 只做 TCP+TLS 握手并放回连接池，**不发送任何 HTTP 请求**，因此不改变
 * keep=false/截断/重试语义。失败静默；不新增线程（由既有线程空闲时调用）。
 * 回滚：-DVELA_TLS_PRECONNECT=0（预连接失效，其余计时日志保留）。 */
#ifndef VELA_TLS_PRECONNECT
#define VELA_TLS_PRECONNECT 1
#endif
/* 预连接硬超时（毫秒）：握手在**不持 s_post_lock** 的情况下进行，socket 读/
 * 写超时被压到剩余预算，mbedtls_ssl_handshake 循环里再查一次 deadline，
 * 保证一次预连接最多阻塞约该时长，不会让首轮用户消息卡很久。 */
#ifndef VELA_TLS_PRECONNECT_TIMEOUT_MS
#define VELA_TLS_PRECONNECT_TIMEOUT_MS 2000
#endif

/* 最近一次“新建连接”的耗时（毫秒）与是否复用池连接；供 [lat] 聚合日志读取。 */
static volatile int64_t s_last_conn_ms;
static volatile int64_t s_last_handshake_ms;
static volatile bool s_last_pool_hit;
/* P0-NET：新建连接序号（每成功新建一条 +1）。agent_loop/llm_proxy 用它判断
 * "本轮是否真的新建了连接"：序号不变 => 复用池连接，[lat] 里 conn/hs 记 0/cache。 */
static volatile uint64_t s_conn_seq;
/* 预连接专用硬截止（0=非预连接）；只在 tls_ctx_connect 里读取。 */
static volatile int64_t s_preconnect_deadline_ms;

/* 2026-11-01 v102b：串行化 HTTPS POST。
 * 真机复现：TTS 拉流（mimo POST）与 LLM 请求（同一 host、两个 TLS 连接）
 * 同时进行时，zblue 反复报 "Unable to allocate buffer within timeout"，
 * 随后整机冻死（无断言、看门狗复位）。把两个 POST 串行化，避免 BT
 * PAN 上并发 TLS 流把小缓冲池打爆。只包 POST（LLM/TTS 都走这里）。
 *
 * ★ SPEED-A：锁从 vela_https_post_json() 内部提升到文件作用域，
 * 因为流式 POST（vela_https_post_json_stream）同样必须在**整个流**期间
 * 持锁 —— 否则一条流式 LLM 响应会和 TTS 拉流并发。 */
static pthread_mutex_t s_post_lock = PTHREAD_MUTEX_INITIALIZER;

/* Acquire a slot for host:port.  Returns a locked, connected ctx or NULL. */
static conn_slot_t* pool_acquire(const char* host, const char* port)
{
    pthread_mutex_lock(&s_pool_lock);

    /* v125：上行（PAN/4G）断过 → 池里的 keep-alive 连接已不可信，先全部丢弃。
     * 若不丢：下一次请求会先"复用"一条半开连接，写可能成功、读要等到
     * SO_RCVTIMEO（30s）才失败（tls_read_response 的 drain 只能识别
     * FIN/close_notify，识别不了静默断链）。 */
    if (s_pool_stale) {
        for (int i = 0; i < CONN_POOL_SIZE; i++) {
            if (s_pool[i].valid && !s_pool[i].in_use) {
                syslog(LOG_INFO,
                    "[%s] Pool slot %d dropped (network link down)\n", TAG, i);
                tls_ctx_free(&s_pool[i].ctx);
                s_pool[i].valid = false;
                s_pool[i].host[0] = '\0';
                s_pool[i].reuses = 0;
            }
        }
        s_pool_stale = false;
    }

    /* 1. Find an existing valid slot for this host:port */
    for (int i = 0; i < CONN_POOL_SIZE; i++) {
        conn_slot_t* s = &s_pool[i];
        if (s->valid && !s->in_use && strcmp(s->host, host) == 0 && strcmp(s->port, port) == 0) {
            /* v125：复用上限 2→8、空闲门限 10s→120s。
             *
             * 证据（.v3_work/perf/serial_perf_session.log / serial_ts.log）：
             *  - 每次重连都要重做 TLS 握手，实测耗时 8s/2s/7s/3s/2s/2s
             *    （log:1111→1142、1445→1463、1865→1903、2039→2057、
             *    2326→2346、3083→3106），中位 2.5s，最长 8s；
             *  - "pool slot reuse limit (N) or idle, reconnect" 在相邻两轮
             *    请求只隔 16s/60s/75s 时也出现（serial_ts.log:16:06:22/16:06:39、
             *    perf log:11:51:25→11:52:25），说明 10s 空闲门限把大量本可
             *    复用的连接判死，每轮白付一次握手。
             *
             * v113b 当时把复用限在 2 的原因是"真机 TTS 读取阶段整机停摆"；
             * 该停摆的可解释根因（chunked 响应末尾块未检测，1KB 响应干等读超时）
             * 已在 tls_read_response() 的末尾块检测里修掉（见本文件约 644-671 行），
             * 所以这里可以放宽。上限仍保留：服务器一般 60-300s 后关连接，
             * 到期后由 tls_read_response()/drain 的 EOF/PEER_CLOSE_NOTIFY 检测
             * 或写失败路径触发重连，不会无限复用。 */
            if (s->reuses >= 2 || (tls_mono_ms() - s->last_used_ms) > 10000) {
                syslog(LOG_INFO, "[%s] pool slot reuse limit (%d) or idle, reconnect\n",
                       TAG, s->reuses);
                tls_ctx_free(&s->ctx);
                s->valid = false;
                s->reuses = 0;
                s->in_use = true;
                pthread_mutex_unlock(&s_pool_lock);
                return s;
            }
            s->in_use = true;
            s_last_pool_hit = true;   /* P0-NET：本次请求复用池连接（无握手） */
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
        s->reuses++;                       /* v113b：记录复用次数 */
        s->last_used_ms = tls_mono_ms();
    } else {
        tls_ctx_free(&s->ctx);
        s->valid = false;
        s->host[0] = '\0';
        s->reuses = 0;
    }
    s->in_use = false;
    pthread_mutex_unlock(&s_pool_lock);
}

/* v125：网络上行断开时由 network_manager.c 的 iface_poll_thread 调用。
 * 只置标（任意线程、任意栈大小都安全），空闲连接在下一次请求里回收。 */
void vela_tls_pool_invalidate(void)
{
    s_pool_stale = true;
    syslog(LOG_INFO, "[%s] Pool marked stale (network link down)\n", TAG);
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

/* P0-NET：读取最近一次连接分段耗时；pool_hit=true 表示该次复用了池连接
 * （此时 conn_ms/handshake_ms 是更早一次新建连接的值，仅作参考）。 */
void vela_tls_get_last_timing(int64_t* conn_ms, int64_t* handshake_ms,
    bool* pool_hit)
{
    if (conn_ms != NULL)
        *conn_ms = s_last_conn_ms;
    if (handshake_ms != NULL)
        *handshake_ms = s_last_handshake_ms;
    if (pool_hit != NULL)
        *pool_hit = s_last_pool_hit;
}

/* P0-NET：新建连接序号。调用方在动作前后各取一次，序号变化表示本轮真的
 * 新建了连接（否则为复用池连接 cache）。 */
uint64_t vela_tls_conn_seq(void)
{
    return s_conn_seq;
}

static int tls_ctx_connect(tls_ctx_t* ctx, const char* host, const char* port)
{
    int ret;
    /* P0-NET：分段计时（TCP connect / TLS handshake），日志与 [lat] 聚合使用。 */
    int64_t t_conn0 = tls_mono_ms();
    int64_t t_hs0 = 0;

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
    } else {
        if ((ret = mbedtls_net_connect(&ctx->net, host, port,
                 MBEDTLS_NET_PROTO_TCP))
            != 0) {
            syslog(LOG_ERR, "[%s] net_connect %s:%s ret=0x%x\n",
                TAG, host, port, -ret);
            return VELA_TLS_ERR_CONNECT;
        }
    }

    s_last_conn_ms = tls_mono_ms() - t_conn0;

    /* Force blocking mode and set socket-level read timeout.
     * This avoids using select()/poll() inside mbedtls_net_recv_timeout
     * which returns MBEDTLS_ERR_NET_POLL_FAILED (-0x0047) on NuttX/QEMU.
     * Timeout is configured via AGENT_LLM_SOCKET_TIMEOUT_SEC in
     * agent_config.h.  LLM APIs (especially kimi with thinking mode)
     * can take over 60s for long responses. */
    mbedtls_net_set_block(&ctx->net);
    if (ctx->net.fd >= 0) {
        if (s_preconnect_deadline_ms > 0) {
            /* P0-NET 预连接：握手阶段的读写超时压到“剩余预算”，保证
             * mbedtls_ssl_handshake 的一次阻塞 read 不会超过硬截止。 */
            int64_t remain = s_preconnect_deadline_ms - tls_mono_ms();
            struct timeval pv;

            if (remain <= 0) {
                syslog(LOG_INFO,
                    "[%s] preconnect deadline hit before handshake\n", TAG);
                return VELA_TLS_ERR_CONNECT;
            }
            pv.tv_sec = (time_t)(remain / 1000);
            pv.tv_usec = (suseconds_t)((remain % 1000) * 1000);
            setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &pv, sizeof(pv));
            setsockopt(ctx->net.fd, SOL_SOCKET, SO_SNDTIMEO, &pv, sizeof(pv));
        } else {
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
                setsockopt(ctx->net.fd, SOL_SOCKET, SO_SNDTIMEO, &ctv,
                    sizeof(ctv));
            }
#endif
        }
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

    /* Handshake */
    t_hs0 = tls_mono_ms();
    while ((ret = mbedtls_ssl_handshake(&ctx->ssl)) != 0) {
        if (s_preconnect_deadline_ms > 0
            && tls_mono_ms() > s_preconnect_deadline_ms) {
            /* P0-NET：预连接硬超时（握手被 socket 超时打断或本身太慢）。 */
            syslog(LOG_INFO,
                "[%s] preconnect handshake timeout (>%dms), giving up\n",
                TAG, VELA_TLS_PRECONNECT_TIMEOUT_MS);
            return VELA_TLS_ERR_HANDSHAKE;
        }
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
            return VELA_TLS_ERR_HANDSHAKE;
        }
    }

    s_last_handshake_ms = tls_mono_ms() - t_hs0;
    s_last_pool_hit = false;
    s_conn_seq++;   /* P0-NET：新建连接序号（[lat] 判断本轮是否真的建连） */
    syslog(LOG_INFO,
        "[lat] tls new_conn host=%s conn_ms=%ld handshake_ms=%ld ver=%s\n",
        host, (long)s_last_conn_ms, (long)s_last_handshake_ms,
        mbedtls_ssl_get_version(&ctx->ssl));

    syslog(LOG_INFO, "[%s] Handshake OK: %s / %s\n", TAG, mbedtls_ssl_get_version(&ctx->ssl),
        mbedtls_ssl_get_ciphersuite(&ctx->ssl));

    return 0;
}

/* P0-NET：保守预连接（见文件上方开关说明）。
 * 只做 TCP+TLS 握手并放回池，不发送 HTTP 请求；不新增线程，失败静默。
 *
 * ★ perf-fw 复核修正（构建前必改）：**握手期间不得持 s_post_lock**（否则会
 *   阻塞 LLM/TTS POST）。流程改为：
 *     1) trylock(s_post_lock) 探测 busy；忙 → 直接跳过（宁可不预连接）；
 *     2) 在锁内 pool_acquire() 登记 in_use 槽（防止被其它请求复用/淘汰）；
 *     3) **释放 s_post_lock**；
 *     4) 做 TCP+TLS 握手，socket 读写超时压到剩余预算 + 握手循环查 deadline，
 *        硬上限 VELA_TLS_PRECONNECT_TIMEOUT_MS（默认 2s）；
 *     5) pool_release(keep=true) 放回（预置 reuses=-1，不消耗复用配额）。
 *   TCP connect 本身仍在现有网络连接超时内（PAN 已连接时通常 <1s），
 *   若仍超时由上层（agent_loop 只做一次/受限次数）兜底。 */
int vela_tls_preconnect(const char* host, const char* port)
{
#if VELA_TLS_PRECONNECT
    if (host == NULL || port == NULL || host[0] == '\0' || port[0] == '\0')
        return -1;

#ifdef CONFIG_AI_AGENT_NET_RPMSG
    if (network_get_state() != NET_STATE_CONNECTED) {
        syslog(LOG_DEBUG, "[%s] preconnect skipped: no uplink\n", TAG);
        return -1;
    }
#endif

    /* 1) 只短暂持 POST 锁：探测 busy + 登记 in_use 槽。 */
    if (pthread_mutex_trylock(&s_post_lock) != 0) {
        syslog(LOG_DEBUG, "[%s] preconnect skipped: POST busy\n", TAG);
        return -1;
    }

    conn_slot_t* slot = pool_acquire(host, port);

    if (slot == NULL) {
        pthread_mutex_unlock(&s_post_lock);
        return -1;
    }

    if (slot->valid && strcmp(slot->host, host) == 0
        && strcmp(slot->port, port) == 0) {
        /* 池里已有同一 host:port 的暖连接：直接放回。 */
        pool_release(slot, host, port, true);
        syslog(LOG_DEBUG, "[%s] preconnect: pool already warm %s:%s\n",
            TAG, host, port);
        pthread_mutex_unlock(&s_post_lock);
        return 0;
    }

    /* 2) 槽已登记为 in_use：立即释放 POST 锁，握手期间不阻塞任何 POST。 */
    pthread_mutex_unlock(&s_post_lock);

    /* 3) 硬截止握手（socket 超时 + 握手循环 deadline 双重约束）。 */
    s_preconnect_deadline_ms = tls_mono_ms() + VELA_TLS_PRECONNECT_TIMEOUT_MS;
    int ret = tls_ctx_connect(&slot->ctx, host, port);
    s_preconnect_deadline_ms = 0;

    if (ret != 0) {
        syslog(LOG_INFO, "[%s] preconnect %s:%s failed (silent, ret=%d)\n",
            TAG, host, port, ret);
        pool_release(slot, host, port, false);
        return -1;
    }

    /* 4) 放回池：pool_release(keep=true) 写 host/port/valid 并 ++reuses；
     *    预置 -1 使预连接本身不消耗"复用次数"配额。 */
    slot->reuses = -1;
    pool_release(slot, host, port, true);

    syslog(LOG_INFO, "[lat] preconnect ok %s:%s conn_ms=%ld handshake_ms=%ld\n",
        host, port, (long)s_last_conn_ms, (long)s_last_handshake_ms);
    return 0;
#else
    (void)host;
    (void)port;
    return -1;
#endif
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
    /* 保持 keep-alive: chunked 响应"读不结束"的问题改在 tls_read_response 里
     * 用末尾块("\r\n0\r\n\r\n")检测解决 —— 既不等对端关连接, 又保留
     * 连接复用(省一次 TLS 握手)。 */
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
    int read_calls = 0;
    struct timespec t_start;

    clock_gettime(CLOCK_MONOTONIC, &t_start);

    /* Read until we have the full header (double CRLF) or buffer full */
    while (!eof && raw_len < TLS_RAW_BUF_SIZE - 1) {
        ret = mbedtls_ssl_read(&ctx->ssl,
            (unsigned char*)(raw + raw_len),
            TLS_RAW_BUF_SIZE - 1 - raw_len);
        if (ret > 0) {
            raw_len += (size_t)ret;
            read_calls++;
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

    /* chunked 且首个数据块里已经带完整末尾块: 直接解码, 不再读 socket
     * (否则会阻塞到对端关闭/读超时, 真机表现为 1KB 响应卡 20s)。 */
    if (chunked && resp_pos > 8
        && memmem(resp_buf, resp_pos, "\r\n0\r\n\r\n", 7)) {
        eof = 1;
    }

    /* Keep reading body.
     * v142 Fix1：旧代码对 0/PEER_CLOSE/任意 mbedtls 错误一律静默 break，
     * 把"读到一半失败"伪装成正常结束（HTTP 200 + 半截 body）。现在区分：
     *   peer_closed —— 对端关闭（body 收完整才算正常，否则 truncated）
     *   read_err    —— 真正的读错误，记录错误码供取证
     * 完整性在循环后用 Content-Length / chunked 终结符校验。 */
    int read_err = 0;
    int peer_closed = 0;

    if (!eof) {
        while (resp_pos < resp_cap - 1) {
            if (content_length >= 0 && (long)resp_pos >= content_length)
                break;
            if (chunked && chunked_end_seen(resp_buf, resp_pos))
                break;
            ret = mbedtls_ssl_read(&ctx->ssl,
                (unsigned char*)(resp_buf + resp_pos),
                resp_cap - 1 - resp_pos);
            if (ret > 0) {
                resp_pos += (size_t)ret;
                read_calls++;

                if (chunked && chunked_end_seen(resp_buf, resp_pos))
                    break;
            } else if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                peer_closed = 1;
                break;
            } else if (ret != MBEDTLS_ERR_SSL_WANT_READ) {
                read_err = ret;
                break;
            }
        }
    }

    /* v142 Fix1：body 完整性校验。任何 framing 没读完都算 truncated：
     *   - Content-Length：resp_pos < cl
     *   - chunked：没看到 "0\r\n\r\n" 终结块
     *   - 无 framing（HTTP/1.0 close-delimited）：EOF 之前出现读错误
     * 返回 VELA_TLS_ERR_TRUNCATED 并 keep=false（不把带残留 body 的连接
     * 放回池）；**不在这里重发 POST**（见 vela_tls.h 错误码注释）。 */
    {
        int truncated = 0;
        /* 调用方缓冲（resp_cap）已满导致的截断不算网络截断：保持旧语义
         *（有些调用方故意传小缓冲，只关心前 N 字节；若在这里报错会改变它们
         * 的行为）。只记录一行便于发现"明明有更多数据却没读"的配置问题。 */
        int capped = (resp_pos >= resp_cap - 1);

        if (!capped) {
            if (content_length >= 0 && (long)resp_pos < content_length)
                truncated = 1;
            else if (chunked && !chunked_end_seen(resp_buf, resp_pos))
                truncated = 1;
            else if (content_length < 0 && !chunked && read_err != 0)
                truncated = 1;
        } else if ((content_length >= 0 && (long)resp_pos < content_length)
                   || (chunked && !chunked_end_seen(resp_buf, resp_pos))) {
            syslog(LOG_INFO,
                   "[%s] body capped by caller buffer (%u/%ld), keep old truncate semantics\n",
                   TAG, (unsigned)resp_pos, content_length);
        }

        if (truncated) {
            struct timespec t_end;
            long ms;

            clock_gettime(CLOCK_MONOTONIC, &t_end);
            ms = (long)(t_end.tv_sec - t_start.tv_sec) * 1000L
               + (t_end.tv_nsec - t_start.tv_nsec) / 1000000L;

            if (read_err != 0) {
                /* 单独一行 -0x%04x：真机排障直接 grep，可对上 mbedtls 手册 */
                syslog(LOG_WARNING,
                       "[%s] body read error -0x%04x at %u/%ld chunked=%d %ldms\n",
                       TAG, -read_err, (unsigned)resp_pos, content_length,
                       chunked, ms);
            }
            syslog(LOG_WARNING,
                   "[%s] body TRUNCATED %s%u/%ld chunked=%d peer_closed=%d reads=%d %ldms status=%d\n",
                   TAG, read_err != 0 ? "after-error " : "",
                   (unsigned)resp_pos, content_length, chunked, peer_closed,
                   read_calls, ms, http_status);

            if (out_body_len)
                *out_body_len = resp_pos;
            if (out_keep_alive)
                *out_keep_alive = false;   /* 连接不可复用，不放回池 */

            tls_raw_release(raw);
            return VELA_TLS_ERR_TRUNCATED;
        }
    }

    /* 对端已关闭：即使 body 完整，也不能再放回连接池复用。 */
    if (peer_closed && out_keep_alive)
        *out_keep_alive = false;

    resp_buf[resp_pos] = '\0';
    tls_raw_release(raw);

    /* Chunked decode */
    if (chunked) {
        resp_pos = decode_chunked(resp_buf, resp_pos);
        resp_buf[resp_pos] = '\0';
    }

    if (out_body_len)
        *out_body_len = resp_pos;

    /* 慢网吞吐取证: 大响应打一行"读了几次/耗时/编码", 便于判断是链路速率
     * 还是"等对端关闭"的空等。 */
    if (resp_pos > 2048) {
        struct timespec t_end;
        long ms;

        clock_gettime(CLOCK_MONOTONIC, &t_end);
        ms = (long)(t_end.tv_sec - t_start.tv_sec) * 1000L
           + (t_end.tv_nsec - t_start.tv_nsec) / 1000000L;

        syslog(LOG_INFO,
               "[%s] body=%u reads=%d %ldms chunked=%d cl=%ld status=%d\n",
               TAG, (unsigned)resp_pos, read_calls, ms, chunked,
               content_length, http_status);
    }

    return http_status;
}


/* ── Streaming response (incremental body, SSE-friendly) ───────
 *
 * vela_https_post_json_stream() 需要"边收边交"而不是先攒完整个 body。
 * 复用上面的连接池 / tls_write_request() / keep-alive 规则，只把
 * tls_read_response() 换成下面的流式版本：
 *   - 先读响应头（仍用 8KB 静态 raw buffer，解析完立即归还）；
 *   - body 按 Transfer-Encoding: chunked / Content-Length 两种 framing
 *     增量解码，解出的载荷片段直接回调，不落大缓冲；
 *   - 两级超时：整体 deadline（调用方传 timeout_ms）+ 每次读的
 *     SO_RCVTIMEO（流式期间临时压到 20s，死链比 60s 更快失败）；
 *   - 每轮读之前查 BT PAN/4G 链路状态，掉线立即返回负错误（不再干等）。
 */

#define TLS_STREAM_IDLE_TIMEOUT_SEC 20
#define TLS_STREAM_CHUNK_BUF        512

/* chunked 解码状态（流式版，逐片段喂入） */
#define CHUNK_ST_SIZE      0  /* 正在收 <hex-size>\r\n 行 */
#define CHUNK_ST_DATA      1  /* 正在收本 chunk 的载荷 */
#define CHUNK_ST_DATA_CRLF 2  /* 载荷后的 \r\n */
#define CHUNK_ST_END_CRLF  3  /* 末尾 "0" chunk 之后的 \r\n */
#define CHUNK_ST_DONE      4  /* 一条完整 chunked body 收完 */

typedef struct {
    int state;
    size_t chunk_left;
    char sizebuf[16];
    size_t size_len;
    int crlf_seen;
} chunk_stream_t;

/* 等价 decode_chunked() 的流式版本：raw 是仍带 chunk 帧的字节，
 * 解出的载荷交给 cb。cb 返回 >0 表示调用方要提前结束。
 * 返回 0 正常；负值为解析/回调错误。 */
static int chunk_stream_feed(chunk_stream_t* cs, vela_http_stream_cb_t cb,
    void* arg, const char* data, size_t len, size_t* out_payload,
    int* out_stop)
{
    size_t i = 0;

    while (i < len && cs->state != CHUNK_ST_DONE) {
        switch (cs->state) {
        case CHUNK_ST_SIZE: {
            char c = data[i++];

            if (c == '\r')
                break;
            if (c == '\n') {
                char* endp = NULL;
                long sz;

                cs->sizebuf[cs->size_len] = '\0';
                sz = strtol(cs->sizebuf, &endp, 16);
                if (endp == cs->sizebuf || sz < 0
                    || sz > (long)(1024 * 1024))
                    return -EIO;
                cs->size_len = 0;
                if (sz == 0) {
                    cs->state = CHUNK_ST_END_CRLF;
                    cs->crlf_seen = 0;
                } else {
                    cs->chunk_left = (size_t)sz;
                    cs->state = CHUNK_ST_DATA;
                }
            } else {
                if (cs->size_len >= sizeof(cs->sizebuf) - 1)
                    return -EIO;
                cs->sizebuf[cs->size_len++] = c;
            }
            break;
        }
        case CHUNK_ST_DATA: {
            size_t n = len - i;
            int cbr;

            if (cs->chunk_left == 0) {
                cs->state = CHUNK_ST_DATA_CRLF;
                cs->crlf_seen = 0;
                break;
            }
            if (n > cs->chunk_left)
                n = cs->chunk_left;
            cbr = cb(data + i, n, arg);
            if (cbr < 0)
                return cbr;
            if (cbr > 0) {
                *out_stop = 1;
                return 0;
            }
            *out_payload += n;
            i += n;
            cs->chunk_left -= n;
            if (cs->chunk_left == 0) {
                cs->state = CHUNK_ST_DATA_CRLF;
                cs->crlf_seen = 0;
            }
            break;
        }
        case CHUNK_ST_DATA_CRLF:
        case CHUNK_ST_END_CRLF: {
            char expect = (cs->crlf_seen == 0) ? '\r' : '\n';

            if (data[i++] != expect)
                return -EIO;
            cs->crlf_seen++;
            if (cs->crlf_seen == 2) {
                cs->crlf_seen = 0;
                if (cs->state == CHUNK_ST_END_CRLF)
                    cs->state = CHUNK_ST_DONE;
                else
                    cs->state = CHUNK_ST_SIZE;
            }
            break;
        }
        default:
            return -EIO;
        }
    }

    return 0;
}

/* BT PAN / 4G 掉线检测：流式期间每轮读之前查一次，掉线立即失败。
 * 没有 NET_RPMSG 时退化为 false（走 socket 超时兜底）。 */
static bool tls_stream_link_down(void)
{
#ifdef CONFIG_AI_AGENT_NET_RPMSG
    return network_get_state() != NET_STATE_CONNECTED;
#else
    return false;
#endif
}

/* ── VOICE-A（2026-11-04）：流式读超时按"距整体 deadline 的剩余时间"收紧 ──
 * 旧实现只在两次 read 之间检查 deadline，body 阶段的 SO_RCVTIMEO 固定 20s
 * （响应头阶段用 connect 时设的 60s）——服务端一个字节都不发时，一次阻塞
 * read 就能越过 deadline（语音 8s 首 token 超时会被拖到 ~20/60s）。
 *
 * 新实现（默认开启，可编译开关回滚）：
 *   - 响应头阶段：单次读超时 = min(连接时超时, 剩余 deadline)
 *     （非语音 LLM 90s > 60s、TTS 30s：行为与原一致或更早失败，不超过整体 deadline）
 *   - body 阶段：单次读超时 = min(TLS_STREAM_IDLE_TIMEOUT_SEC(20s), 剩余 deadline)
 *     （90s/30s 总预算的既有调用方仍是原来的 20s 空闲超时，行为不变；
 *       语音 8s 总预算 => 每次读最多 8s，第一次 read 就能在 deadline 内失败）
 *   - 函数返回前恢复连接原超时值，避免影响池化复用的后续请求。
 * 回滚：-DVELA_TLS_STREAM_DEADLINE_READ_TIMEOUT=0（恢复旧的固定超时行为）。 */
#ifndef VELA_TLS_STREAM_DEADLINE_READ_TIMEOUT
#define VELA_TLS_STREAM_DEADLINE_READ_TIMEOUT 1
#endif

#if VELA_TLS_STREAM_DEADLINE_READ_TIMEOUT
/* 读取连接当前 SO_RCVTIMEO（毫秒）；0 表示无限/读失败。 */
static int64_t tls_stream_sock_timeout_ms(tls_ctx_t* ctx, struct timeval* old)
{
    socklen_t olen = sizeof(*old);

    memset(old, 0, sizeof(*old));
    if (ctx->net.fd < 0)
        return 0;
    if (getsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, old, &olen) != 0)
        return 0;
    if (old->tv_sec <= 0 && old->tv_usec <= 0)
        return 0;
    return (int64_t)old->tv_sec * 1000 + old->tv_usec / 1000;
}

static void tls_stream_restore_sock_timeout(tls_ctx_t* ctx,
    const struct timeval* old)
{
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };

    if (ctx->net.fd < 0)
        return;
    if (old->tv_sec > 0 || old->tv_usec > 0)
        tv = *old;
    /* old 为 0 = 原值是无限等待；显式写回 0，别让收紧后的值泄漏到池化连接。 */
    setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

/* 设置下一次 read 的超时：min(cap_ms, 距整体 deadline 的剩余)。
 * 返回 0 成功；-1 表示 deadline 已到（调用方按 -ETIMEDOUT 处理）。 */
static int tls_stream_arm_read_timeout(tls_ctx_t* ctx, int64_t deadline_ms,
    int64_t cap_ms)
{
    int64_t remain = deadline_ms - tls_mono_ms();
    struct timeval tv;

    if (remain <= 0)
        return -1;
    if (cap_ms > 0 && remain > cap_ms)
        remain = cap_ms;

    tv.tv_sec = (time_t)(remain / 1000);
    tv.tv_usec = (suseconds_t)((remain % 1000) * 1000);
    if (ctx->net.fd >= 0)
        setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    return 0;
}
#else /* 旧行为：body 阶段固定 20s 读超时 */
static void tls_stream_timeout_push(tls_ctx_t* ctx, struct timeval* old)
{
    struct timeval tv = { .tv_sec = TLS_STREAM_IDLE_TIMEOUT_SEC,
        .tv_usec = 0 };
    socklen_t olen = sizeof(*old);

    memset(old, 0, sizeof(*old));
    if (ctx->net.fd < 0)
        return;
    getsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, old, &olen);
    setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

static void tls_stream_timeout_pop(tls_ctx_t* ctx, const struct timeval* old)
{
    if (ctx->net.fd < 0 || old->tv_sec <= 0)
        return;
    setsockopt(ctx->net.fd, SOL_SOCKET, SO_RCVTIMEO, old, sizeof(*old));
}
#endif /* VELA_TLS_STREAM_DEADLINE_READ_TIMEOUT */

/**
 * 流式读 HTTP/1.1 响应：只收头 + 增量转发 body。
 * 返回：成功时 HTTP 状态码（>0）；失败返回负 VELA_TLS_ERR_* / -EIO / -ETIMEDOUT。
 * out_keep_alive：连接是否可放回池（body 完整读完才 true）。
 */
/* RCA-FIX-A（audio_rca2 F1）：首字节前把有效 deadline 收紧到
 * first_body_deadline_ms（绝对单调时刻，0=不启用）。首个 body 字节到达后恢复
 * 只看整体 deadline。 */
static int64_t tls_stream_effective_deadline(int64_t deadline_ms,
    int64_t first_body_deadline_ms, bool first_body_seen)
{
    if (!first_body_seen && first_body_deadline_ms > 0
        && first_body_deadline_ms < deadline_ms)
        return first_body_deadline_ms;
    return deadline_ms;
}

/* RCA-FIX-A F8：记录首个 body 字节到达时刻（只有第一次有效）。 */
static void tls_log_first_byte(bool* seen, int64_t t_start_ms, size_t bytes)
{
    if (*seen)
        return;
    *seen = true;
    syslog(LOG_INFO, "[%s] [lat] http first_byte_ms=%ld bytes=%zu\n",
        TAG, (long)(tls_mono_ms() - t_start_ms), bytes);
}

/* RCA-FIX-A：流式读失败归类。
 *   首个 body 字节前到达 first_body deadline → -ETIMEDOUT（主动首 token 超时）；
 *   否则按原整体 deadline/READ 错误语义。 */
static int tls_stream_read_error(int mbedtls_ret, int64_t t_start_ms,
    int64_t deadline_ms, int64_t first_body_deadline_ms, bool first_body_seen)
{
    int64_t now = tls_mono_ms();

    if (!first_body_seen && first_body_deadline_ms > 0
        && now >= first_body_deadline_ms) {
        syslog(LOG_WARNING,
            "[%s] [lat] stream first_body deadline hit after %ldms "
            "(read err -0x%04x)\n",
            TAG, (long)(now - t_start_ms), -mbedtls_ret);
        return -ETIMEDOUT;
    }
    if (now >= deadline_ms) {
        syslog(LOG_WARNING,
            "[%s] stream read deadline hit after %ldms (-0x%04x)\n",
            TAG, (long)(now - t_start_ms), -mbedtls_ret);
        return -ETIMEDOUT;
    }
    syslog(LOG_WARNING,
        "[%s] stream body read error -0x%04x after %ldms\n",
        TAG, -mbedtls_ret, (long)(now - t_start_ms));
    return VELA_TLS_ERR_READ;
}

static int tls_read_response_stream(tls_ctx_t* ctx,
    vela_http_stream_cb_t cb, void* arg,
    size_t* out_body_len, bool* out_keep_alive, int64_t deadline_ms,
    int64_t first_body_deadline_ms)
{
    char* raw = tls_raw_acquire();
    size_t raw_len = 0;
    size_t payload = 0;
    int http_status = 0;
    int err = 0;
    int stop = 0;
    bool ok = false;
    bool keep = false;
    bool first_body_seen = false;
    long content_length = -1;
    int chunked = 0;
    int64_t t_start_ms = tls_mono_ms();
    char buf[TLS_STREAM_CHUNK_BUF];
#if !VELA_TLS_STREAM_DEADLINE_READ_TIMEOUT
    struct timeval old_tv;
    bool timeout_pushed = false;
#endif
#if VELA_TLS_STREAM_DEADLINE_READ_TIMEOUT
    struct timeval orig_tv;
    int64_t orig_timeout_ms = 0;

    /* VOICE-A：保存连接原 SO_RCVTIMEO，函数返回前恢复（池化连接不能被改短）。 */
    orig_timeout_ms = tls_stream_sock_timeout_ms(ctx, &orig_tv);
    if (orig_timeout_ms <= 0)
        orig_timeout_ms = (int64_t)AGENT_LLM_SOCKET_TIMEOUT_SEC * 1000;
#endif
    chunk_stream_t cs;

    /* ── 1. 读响应头（到 \r\n\r\n）── */
    for (;;) {
        int r;
        int64_t eff_deadline = tls_stream_effective_deadline(deadline_ms,
            first_body_deadline_ms, first_body_seen);

        if (tls_mono_ms() > eff_deadline) {
            if (eff_deadline != deadline_ms) {
                syslog(LOG_WARNING,
                    "[%s] [lat] stream first_body deadline hit after %ldms "
                    "(headers not complete)\n",
                    TAG, (long)(tls_mono_ms() - t_start_ms));
            }
            err = -ETIMEDOUT;
            goto out;
        }
#if VELA_TLS_STREAM_DEADLINE_READ_TIMEOUT
        /* 响应头阶段也要被整体/首字节 deadline 约束（否则一次 60s 阻塞
         * read 会把语音 8s 首 token 超时拖过 deadline）。 */
        if (tls_stream_arm_read_timeout(ctx, eff_deadline,
                orig_timeout_ms) != 0) {
            err = -ETIMEDOUT;
            goto out;
        }
#endif
        if (tls_stream_link_down()) {
            err = VELA_TLS_ERR_READ;
            goto out;
        }
        if (raw_len >= TLS_RAW_BUF_SIZE - 1) {
            err = VELA_TLS_ERR_OVERFLOW;
            goto out;
        }
        r = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)(raw + raw_len),
            TLS_RAW_BUF_SIZE - 1 - raw_len);
        if (r > 0) {
            raw_len += (size_t)r;
            raw[raw_len] = '\0';
            if (memmem(raw, raw_len, "\r\n\r\n", 4))
                break;
        } else if (r == 0 || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
            /* 连接在响应头之前就断了 */
            err = VELA_TLS_ERR_READ;
            goto out;
        } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
            err = VELA_TLS_ERR_READ;
            goto out;
        }
    }
    raw[raw_len] = '\0';

    if (sscanf(raw, "HTTP/1.%*d %d", &http_status) != 1) {
        err = VELA_TLS_ERR_READ;
        goto out;
    }

    char* body_start = (char*)memmem(raw, raw_len, "\r\n\r\n", 4);
    if (!body_start) {
        /* 只有头没有 \r\n\r\n：连接已结束，按状态码返回 */
        ok = true;
        keep = false;
        goto out;
    }

    /* keep-alive 判定（与 tls_read_response 同规则） */
    keep = true;
    {
        char* conn_hdr = strcasestr(raw, "Connection:");
        if (conn_hdr && conn_hdr < body_start)
            keep = (strcasestr(conn_hdr, "keep-alive") != NULL);
    }
    {
        char* cl_hdr = strcasestr(raw, "Content-Length:");
        if (cl_hdr && cl_hdr < body_start) {
            content_length = strtol(cl_hdr + strlen("Content-Length:"),
                NULL, 10);
            if (content_length < 0 || content_length > 10 * 1024 * 1024)
                content_length = -1;
        }
    }
    {
        char* te_hdr = strcasestr(raw, "Transfer-Encoding:");
        if (te_hdr && te_hdr < body_start)
            chunked = (strcasestr(te_hdr, "chunked") != NULL);
    }
    body_start += 4;

    /* 非 200：不转 body（错误体很小），由上层回退非流式重发。
     * 连接不放回池（body 还没读）。 */
    if (http_status != 200) {
        ok = true;
        keep = false;
        goto out;
    }

    /* ── 2. body：先处理响应头里已经带出来的首段，再续读 ── */
#if !VELA_TLS_STREAM_DEADLINE_READ_TIMEOUT
    tls_stream_timeout_push(ctx, &old_tv);
    timeout_pushed = true;
#endif

    memset(&cs, 0, sizeof(cs));
    cs.state = CHUNK_ST_SIZE;

    {
        size_t init = (size_t)(raw + raw_len - body_start);

        if (init > 0)
            tls_log_first_byte(&first_body_seen, t_start_ms, init);

        if (chunked) {
            int fret = chunk_stream_feed(&cs, cb, arg, body_start, init,
                &payload, &stop);
            if (fret < 0) {
                err = fret;
                goto out;
            }
            if (cs.state == CHUNK_ST_DONE) {
                ok = true;
                goto out;
            }

            for (;;) {
                int r;

                int64_t eff_deadline = tls_stream_effective_deadline(
                    deadline_ms, first_body_deadline_ms, first_body_seen);

                if (tls_mono_ms() > eff_deadline) {
                    if (eff_deadline != deadline_ms) {
                        syslog(LOG_WARNING,
                            "[%s] [lat] stream first_body deadline hit after "
                            "%ldms\n", TAG,
                            (long)(tls_mono_ms() - t_start_ms));
                    }
                    err = -ETIMEDOUT;
                    goto out;
                }
                if (tls_stream_link_down()) {
                    err = VELA_TLS_ERR_READ;
                    goto out;
                }
#if VELA_TLS_STREAM_DEADLINE_READ_TIMEOUT
                if (tls_stream_arm_read_timeout(ctx, eff_deadline,
                        (int64_t)TLS_STREAM_IDLE_TIMEOUT_SEC * 1000) != 0) {
                    err = -ETIMEDOUT;
                    goto out;
                }
#endif
                if (stop) {
                    /* 调用方已拿到想要的（如 [DONE]）：提前结束。
                     * 剩余帧没读，连接不能复用。 */
                    ok = true;
                    keep = false;
                    goto out;
                }
                r = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)buf,
                    sizeof(buf));
                if (r > 0) {
                    tls_log_first_byte(&first_body_seen, t_start_ms, (size_t)r);
                    fret = chunk_stream_feed(&cs, cb, arg, buf, (size_t)r,
                        &payload, &stop);
                    if (fret < 0) {
                        err = fret;
                        goto out;
                    }
                    if (cs.state == CHUNK_ST_DONE) {
                        ok = true;
                        goto out;
                    }
                } else if (r == 0
                           || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                    /* 末尾 chunk 之前就 EOF = 响应被截断 */
                    err = VELA_TLS_ERR_READ;
                    goto out;
                } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
                    /* RCA-FIX-A：首字节 deadline 到点 -> -ETIMEDOUT；
                     * 否则沿用原整体 deadline / READ 错误语义。 */
                    err = tls_stream_read_error(r, t_start_ms, deadline_ms,
                        first_body_deadline_ms, first_body_seen);
                    goto out;
                }
            }
        } else if (content_length >= 0) {
            size_t need = (size_t)content_length;
            size_t take = init < need ? init : need;

            if (take > 0) {
                int cbr = cb(body_start, take, arg);
                if (cbr < 0) {
                    err = cbr;
                    goto out;
                }
                if (cbr > 0)
                    stop = 1;
                payload += take;
                need -= take;
            }
            while (!stop && need > 0) {
                int r;

                int64_t eff_deadline = tls_stream_effective_deadline(
                    deadline_ms, first_body_deadline_ms, first_body_seen);

                if (tls_mono_ms() > eff_deadline) {
                    if (eff_deadline != deadline_ms) {
                        syslog(LOG_WARNING,
                            "[%s] [lat] stream first_body deadline hit after "
                            "%ldms\n", TAG,
                            (long)(tls_mono_ms() - t_start_ms));
                    }
                    err = -ETIMEDOUT;
                    goto out;
                }
                if (tls_stream_link_down()) {
                    err = VELA_TLS_ERR_READ;
                    goto out;
                }
#if VELA_TLS_STREAM_DEADLINE_READ_TIMEOUT
                if (tls_stream_arm_read_timeout(ctx, eff_deadline,
                        (int64_t)TLS_STREAM_IDLE_TIMEOUT_SEC * 1000) != 0) {
                    err = -ETIMEDOUT;
                    goto out;
                }
#endif
                r = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)buf,
                    sizeof(buf));
                if (r > 0) {
                    size_t n = (size_t)r;

                    tls_log_first_byte(&first_body_seen, t_start_ms, n);
                    int cbr;

                    if (n > need)
                        n = need;
                    cbr = cb(buf, n, arg);
                    if (cbr < 0) {
                        err = cbr;
                        goto out;
                    }
                    if (cbr > 0)
                        stop = 1;
                    payload += n;
                    need -= n;
                } else if (r == 0
                           || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                    break;
                } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
                    /* RCA-FIX-A：首字节 deadline 到点 -> -ETIMEDOUT；
                     * 否则沿用原整体 deadline / READ 错误语义。 */
                    err = tls_stream_read_error(r, t_start_ms, deadline_ms,
                        first_body_deadline_ms, first_body_seen);
                    goto out;
                }
            }
            if (stop || need == 0) {
                ok = true;
                keep = (need == 0 && !stop) ? keep : false;
                goto out;
            }
            /* Content-Length 没读满 = 截断 */
            err = VELA_TLS_ERR_READ;
            goto out;
        } else {
            /* 既无 chunked 也无 Content-Length：读到 EOF 为止
             * （HTTP/1.1 下少见；SSE 服务端基本都是 chunked）。 */
            if (init > 0) {
                int cbr = cb(body_start, init, arg);
                if (cbr < 0) {
                    err = cbr;
                    goto out;
                }
                payload += init;
                if (cbr > 0)
                    stop = 1;
            }
            while (!stop) {
                int r;

                int64_t eff_deadline = tls_stream_effective_deadline(
                    deadline_ms, first_body_deadline_ms, first_body_seen);

                if (tls_mono_ms() > eff_deadline) {
                    if (eff_deadline != deadline_ms) {
                        syslog(LOG_WARNING,
                            "[%s] [lat] stream first_body deadline hit after "
                            "%ldms\n", TAG,
                            (long)(tls_mono_ms() - t_start_ms));
                    }
                    err = -ETIMEDOUT;
                    goto out;
                }
                if (tls_stream_link_down()) {
                    err = VELA_TLS_ERR_READ;
                    goto out;
                }
#if VELA_TLS_STREAM_DEADLINE_READ_TIMEOUT
                if (tls_stream_arm_read_timeout(ctx, eff_deadline,
                        (int64_t)TLS_STREAM_IDLE_TIMEOUT_SEC * 1000) != 0) {
                    err = -ETIMEDOUT;
                    goto out;
                }
#endif
                r = mbedtls_ssl_read(&ctx->ssl, (unsigned char*)buf,
                    sizeof(buf));
                if (r > 0) {
                    int cbr = cb(buf, (size_t)r, arg);

                    tls_log_first_byte(&first_body_seen, t_start_ms, (size_t)r);
                    if (cbr < 0) {
                        err = cbr;
                        goto out;
                    }
                    payload += (size_t)r;
                    if (cbr > 0)
                        stop = 1;
                } else if (r == 0
                           || r == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY) {
                    break;
                } else if (r != MBEDTLS_ERR_SSL_WANT_READ) {
                    /* RCA-FIX-A：首字节 deadline 到点 -> -ETIMEDOUT；
                     * 否则沿用原整体 deadline / READ 错误语义。 */
                    err = tls_stream_read_error(r, t_start_ms, deadline_ms,
                        first_body_deadline_ms, first_body_seen);
                    goto out;
                }
            }
            ok = true;
            keep = false;   /* 没有 framing，无法确认连接可复用 */
            goto out;
        }
    }

out:
#if VELA_TLS_STREAM_DEADLINE_READ_TIMEOUT
    /* VOICE-A：恢复连接原始读超时，避免收紧后的值泄漏到池化复用的下一请求。 */
    tls_stream_restore_sock_timeout(ctx, &orig_tv);
#else
    if (timeout_pushed)
        tls_stream_timeout_pop(ctx, &old_tv);
#endif
    tls_raw_release(raw);
    if (out_body_len)
        *out_body_len = payload;
    if (out_keep_alive)
        *out_keep_alive = (ok && keep && !stop);
    if (err != 0)
        return err;
    return ok ? http_status : VELA_TLS_ERR_READ;
}

/* RCA-FIX-A F8：请求 body 全部写完的时刻（相对本次流式请求开始）。
 * 在 BT PAN 上 TLS 写完成时间可反推 HCI ACL TX 背压（zblue 的
 * "Unable to allocate buffer within timeout" 无法在本层计数，见报告说明）。 */
static void tls_log_request_sent(const char* host, const char* path,
    size_t body_len, int64_t t_start_ms)
{
    syslog(LOG_INFO,
        "[%s] [lat] http request_sent host=%s path=%s body=%zu sent_ms=%ld\n",
        TAG, host, path, body_len, (long)(tls_mono_ms() - t_start_ms));
}

/* 一次完整的"流式 HTTPS 请求"（连接/握手失败时由公开 wrapper 重试一次，
 * 与 vela_https_request_once 的策略一致：只重试请求还没写出去的失败）。 */
static int vela_https_request_stream_once(
    const char* host,
    const char* port,
    const char* path,
    const vela_header_t* headers,
    const char* body,
    size_t body_len,
    vela_http_stream_cb_t cb,
    void* cb_arg,
    int64_t deadline_ms,
    int64_t first_body_deadline_ms,
    size_t* out_body_len)
{
    int ret;
    int64_t t_start_ms = tls_mono_ms();
    conn_slot_t* slot = pool_acquire(host, port);

    if (slot && slot->valid) {
        if (slot->ctx.net.fd >= 0) {
            unsigned char drain[512];
            int dr;
            struct timeval tv_drain = { .tv_sec = 0, .tv_usec = 10000 };
            struct timeval tv_orig = { .tv_sec = AGENT_LLM_SOCKET_TIMEOUT_SEC,
                .tv_usec = 0 };

            setsockopt(slot->ctx.net.fd, SOL_SOCKET, SO_RCVTIMEO,
                &tv_drain, sizeof(tv_drain));
            while ((dr = mbedtls_ssl_read(&slot->ctx.ssl, drain,
                        sizeof(drain)))
                > 0)
                ;
            setsockopt(slot->ctx.net.fd, SOL_SOCKET, SO_RCVTIMEO,
                &tv_orig, sizeof(tv_orig));
            if (dr == 0 || dr == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
                goto pool_reconnect;
        } else {
            goto pool_reconnect;
        }

        ret = tls_write_request(&slot->ctx, "POST", host, path, headers,
            body, body_len);
        if (ret == 0) {
            bool keep = false;

            tls_log_request_sent(host, path, body_len, t_start_ms);
            ret = tls_read_response_stream(&slot->ctx, cb, cb_arg,
                out_body_len, &keep, deadline_ms, first_body_deadline_ms);
            pool_release(slot, host, port, ret > 0 && keep);
            return ret;
        }
        /* 写失败：连接已不可信 */
    pool_reconnect:
        syslog(LOG_INFO, "[%s] (stream) pooled connection stale, reconnecting\n",
            TAG);
        tls_ctx_free(&slot->ctx);
        slot->valid = false;
    }

    if (!slot) {
        tls_ctx_t tmp_ctx;

        if ((ret = tls_ctx_connect(&tmp_ctx, host, port)) != 0) {
            tls_ctx_free(&tmp_ctx);
            return ret;
        }
        if ((ret = tls_write_request(&tmp_ctx, "POST", host, path, headers,
                 body, body_len))
            != 0) {
            syslog(LOG_ERR, "[%s] (stream) write request failed: %d\n",
                TAG, ret);
            tls_ctx_free(&tmp_ctx);
            return ret;
        }
        tls_log_request_sent(host, path, body_len, t_start_ms);
        ret = tls_read_response_stream(&tmp_ctx, cb, cb_arg, out_body_len,
            NULL, deadline_ms, first_body_deadline_ms);
        tls_ctx_free(&tmp_ctx);
        return ret;
    }

    if ((ret = tls_ctx_connect(&slot->ctx, host, port)) != 0) {
        pool_release(slot, host, port, false);
        return ret;
    }

    if ((ret = tls_write_request(&slot->ctx, "POST", host, path, headers,
             body, body_len))
        != 0) {
        syslog(LOG_ERR, "[%s] (stream) write request failed: %d\n", TAG, ret);
        pool_release(slot, host, port, false);
        return ret;
    }

    tls_log_request_sent(host, path, body_len, t_start_ms);

    bool keep = false;
    ret = tls_read_response_stream(&slot->ctx, cb, cb_arg, out_body_len,
        &keep, deadline_ms, first_body_deadline_ms);
    pool_release(slot, host, port, ret > 0 && keep);
    return ret;
}

/* ── Public API ──────────────────────────────────────────────── */

/* v125：把"一次完整的 HTTPS 请求"单独拆出来，公开的 vela_https_request()
 * 只负责对"尚未写出请求字节"的连接/握手失败重试一次（见下方 wrapper）。 */
static int vela_https_request_once(
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
            if (ret == VELA_TLS_ERR_TRUNCATED) {
                /* v142 Fix1：请求**已经发出**、body 半截 —— 不能再走下面
                 * 的 "reconnect + 重发" 分支（POST 有副作用：LLM 会重复
                 * 计费、TTS 会重复生成）。丢弃连接，是否重试交给调用方
                 * （MiMo TTS 在 mimo_tts.c 里显式重试一次）。 */
                syslog(LOG_WARNING,
                    "[%s] %s %s truncated body: drop conn, no auto retry\n",
                    TAG, method, host);
                pool_release(slot, host, port, false);
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
        if ((ret = tls_ctx_connect(&tmp_ctx, host, port)) != 0) {
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
    if ((ret = tls_ctx_connect(&slot->ctx, host, port)) != 0) {
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
    int64_t t_start = tls_mono_ms();
    int ret = vela_https_request_once(host, port, method, path, headers,
        body, body_len, resp_buf, resp_cap, out_body_len);
    int64_t elapsed_ms = tls_mono_ms() - t_start;

    /* v125：连接/握手阶段"快速失败"时重试 1 次（只重试 HTTP 请求还没写出去的
     * 失败，所以对非幂等的 POST 也不会重复提交）。
     *
     * 证据：真机 .v3_work/perf/serial_perf_session.log:1626-1628
     *   "[vela_tls] net_connect token-plan-cn.xiaomimimo.com:443 ret=0x42"
     *   -0x42 = MBEDTLS_ERR_NET_SOCKET_FAILED（apps/crypto/mbedtls/mbedtls/
     *   include/mbedtls/net_sockets.h:50），即 socket() 系统调用瞬时失败
     *   （IOB/fd 资源压力；同窗口 zblue 连报 52 条
     *   "Unable to allocate buffer within timeout"）。这次失败让 mimo TTS POST
     *   直接 -1，回退链又跑了 36s（log:1628→1865）才重试同一个后端。
     *   握手阶段发送失败 -0x004e（log:1635）同样是不到 2s 的快速失败。
     *
     * 限定 elapsed_ms < 2000：IOB/连接数限流下的失败会先在
     *   network_acquire_resource() 里等满 connect_timeout=15s 再返回
     *   ERR_CONNECT；那种失败重试只会把 15s 变成 30s，必须排除。
     *
     * 不重试 VELA_TLS_ERR_WRITE/READ：那意味着请求可能已经发出，
     * POST 重发会有副作用（重复计费/重复生成）。
     * 300ms 让瞬时的 IOB/fd 压力先释放。 */
    if ((ret == VELA_TLS_ERR_CONNECT || ret == VELA_TLS_ERR_HANDSHAKE)
        && elapsed_ms < 2000) {
        syslog(LOG_WARNING,
            "[%s] %s %s connect/handshake failed (%d) after %ldms, retry once\n",
            TAG, method, host, ret, (long)elapsed_ms);
        usleep(300 * 1000);
        ret = vela_https_request_once(host, port, method, path, headers,
            body, body_len, resp_buf, resp_cap, out_body_len);
        syslog(LOG_INFO, "[%s] %s %s retry %s (ret=%d)\n", TAG, method, host,
            ret > 0 ? "OK" : "failed", ret);
    }

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
    /* s_post_lock 已提升到文件作用域（流式 POST 也要持同一把锁）。 */

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
    int ret;

    if (pthread_mutex_trylock(&s_post_lock) != 0) {
        syslog(LOG_INFO, "[vela_tls] serialize: another HTTPS POST in flight, waiting...\n");
        pthread_mutex_lock(&s_post_lock);
    }

    ret = vela_https_request(host, port, "POST", path, merged,
        json_body, body_len, resp_buf, resp_cap, NULL);

    pthread_mutex_unlock(&s_post_lock);
    return ret;
}


/* RCA-FIX-A：流式 POST 实现；old API 传 first_body_timeout_ms=0 完全等价旧行为。
 * first_body_timeout_ms>0 时，在**首个 body 字节前**把读超时压到该预算，
 * 服务端完全静默也会在 ~8-9s 返回 -ETIMEDOUT（主动首 token deadline）。 */
static int vela_https_post_json_stream_impl(const char* host, const char* port,
    const char* path, const vela_header_t* extra_headers,
    const char* json_body, vela_http_stream_cb_t cb, void* cb_arg,
    uint32_t timeout_ms, uint32_t first_body_timeout_ms)
{
    /* Build a merged header list: Content-Type first, then caller extras
     * (与 vela_https_post_json 完全一致的请求头构造)。 */
    const int MAX_HDRS = 32;
    vela_header_t merged[MAX_HDRS];
    int n = 0;
    size_t body_len;
    int64_t t_start;
    int64_t deadline_ms;
    int64_t first_body_deadline_ms = 0;
    int ret;

    if (!cb)
        return -EINVAL;

    merged[n++] = (vela_header_t) { "Content-Type", "application/json" };
    if (extra_headers) {
        for (const vela_header_t* h = extra_headers;
             h->name && n < MAX_HDRS - 1; h++) {
            merged[n++] = *h;
        }
    }
    merged[n] = (vela_header_t) { NULL, NULL };

    body_len = json_body ? strlen(json_body) : 0;

    t_start = tls_mono_ms();
    deadline_ms = t_start + (int64_t)(timeout_ms > 0
                                          ? timeout_ms
                                          : (uint32_t)AGENT_LLM_TIMEOUT_SEC
                                              * 1000U);
    if (first_body_timeout_ms > 0)
        first_body_deadline_ms = t_start + (int64_t)first_body_timeout_ms;

    /* 2026-11-01 v102b：与 TTS/其它 POST 串行化。整个流期间持锁
     * （见 s_post_lock 定义处注释）。 */
    if (pthread_mutex_trylock(&s_post_lock) != 0) {
        syslog(LOG_INFO,
            "[vela_tls] serialize: another HTTPS POST in flight, stream waits...\n");
        pthread_mutex_lock(&s_post_lock);
    }

    ret = vela_https_request_stream_once(host, port, path, merged,
        json_body, body_len, cb, cb_arg, deadline_ms,
        first_body_deadline_ms, NULL);

    /* 连接/握手快速失败重试 1 次（请求还没写出去，POST 重发无副作用；
     * 与 vela_https_request() 的策略/门槛完全一致）。 */
    if ((ret == VELA_TLS_ERR_CONNECT || ret == VELA_TLS_ERR_HANDSHAKE)
        && (tls_mono_ms() - t_start) < 2000) {
        syslog(LOG_WARNING,
            "[%s] (stream) POST %s connect/handshake failed (%d) after %ldms, retry once\n",
            TAG, host, ret, (long)(tls_mono_ms() - t_start));
        usleep(300 * 1000);
        /* 重试是新的发送尝试：deadline/首字节预算都从重试时刻重新计。 */
        deadline_ms = tls_mono_ms()
            + (int64_t)(timeout_ms > 0 ? timeout_ms
                                       : (uint32_t)AGENT_LLM_TIMEOUT_SEC * 1000U);
        if (first_body_timeout_ms > 0)
            first_body_deadline_ms = tls_mono_ms()
                + (int64_t)first_body_timeout_ms;
        ret = vela_https_request_stream_once(host, port, path, merged,
            json_body, body_len, cb, cb_arg, deadline_ms,
            first_body_deadline_ms, NULL);
        syslog(LOG_INFO, "[%s] (stream) retry %s (ret=%d)\n", TAG,
            ret > 0 ? "OK" : "failed", ret);
    }

    pthread_mutex_unlock(&s_post_lock);
    return ret;
}

int vela_https_post_json_stream(const char* host, const char* port,
    const char* path, const vela_header_t* extra_headers,
    const char* json_body, vela_http_stream_cb_t cb, void* cb_arg,
    uint32_t timeout_ms)
{
    return vela_https_post_json_stream_impl(host, port, path, extra_headers,
        json_body, cb, cb_arg, timeout_ms, 0);
}

int vela_https_post_json_stream_ex(const char* host, const char* port,
    const char* path, const vela_header_t* extra_headers,
    const char* json_body, vela_http_stream_cb_t cb, void* cb_arg,
    uint32_t timeout_ms, uint32_t first_body_timeout_ms)
{
    return vela_https_post_json_stream_impl(host, port, path, extra_headers,
        json_body, cb, cb_arg, timeout_ms, first_body_timeout_ms);
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
