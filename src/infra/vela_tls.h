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

#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum size of HTTP response header section we buffer internally */
#define VELA_TLS_HDR_BUF  4096

/* Return codes (negative on error, HTTP status code on success) */
#define VELA_TLS_ERR_CONNECT   -1
#define VELA_TLS_ERR_HANDSHAKE -2
#define VELA_TLS_ERR_WRITE     -3
#define VELA_TLS_ERR_READ      -4
#define VELA_TLS_ERR_OVERFLOW  -5

/* v142 Fix1：响应 body 没有按 Content-Length / chunked framing 收完整就被
 * 对端关闭或读错误打断。旧代码会把它当 HTTP 200 + 半截 body 返回，调用方
 * 只能拿到坏 JSON（真机 cl=46119/body=34797 -> mimo_tts bad JSON -> TTS
 * 回退链 -> 看门狗复位，见 .v3_work/plan/reset_rca.md）。
 * 语义：请求**已经发出**，vela_tls 不会自动重发 POST；是否重试由调用方按
 * 业务语义决定（MiMo TTS 重试一次；LLM/ASR 不重发）。 */
#define VELA_TLS_ERR_TRUNCATED -6

/**
 * Extra header entry; pass an array terminated by {NULL, NULL}.
 */
typedef struct {
    const char *name;
    const char *value;
} vela_header_t;

/**
 * Perform a complete HTTPS request and collect the response body.
 *
 * @param host        Hostname (e.g. "api.anthropic.com")
 * @param port        Port string (e.g. "443")
 * @param method      "GET", "POST", etc.
 * @param path        URL path starting with '/' (e.g. "/v1/messages")
 * @param headers     NULL-terminated array of extra request headers, or NULL
 * @param body        Request body bytes, or NULL
 * @param body_len    Length of body; 0 if no body
 * @param resp_buf    Caller-supplied buffer for response body
 * @param resp_cap    Capacity of resp_buf (bytes)
 *
 * @return HTTP status code (200, 400, etc.) on success, negative VELA_TLS_ERR_* on failure
 *
 * The response body is written to resp_buf as a NUL-terminated string.
 * If the body is larger than resp_cap-1, it is silently truncated.
 */
int vela_https_request(
    const char       *host,
    const char       *port,
    const char       *method,
    const char       *path,
    const vela_header_t *headers,   /* NULL-terminated array or NULL */
    const char       *body,
    size_t            body_len,
    char             *resp_buf,
    size_t            resp_cap,
    size_t           *out_body_len  /* optional: actual body bytes written, or NULL */
);

/**
 * Convenience wrapper for GET with no extra headers or body.
 */
int vela_https_get(const char *host, const char *port, const char *path,
                   char *resp_buf, size_t resp_cap);

/**
 * Convenience wrapper for POST with Content-Type: application/json.
 */
int vela_https_post_json(const char *host, const char *port, const char *path,
                         const vela_header_t *extra_headers,
                         const char *json_body,
                         char *resp_buf, size_t resp_cap);

/**
 * Plain HTTP (no TLS) POST with Content-Type: application/json.
 * Used for internal/intranet endpoints that don't support HTTPS.
 */
int vela_http_post_json(const char *host, const char *port, const char *path,
                        const vela_header_t *extra_headers,
                        const char *json_body,
                        char *resp_buf, size_t resp_cap);

/**
 * Send a HEAD request and extract the value of the "Date:" response header.
 *
 * @param host     Hostname (e.g. "api.deepseek.com")
 * @param port     Port string (e.g. "443")
 * @param path     Request path (e.g. "/")
 * @param date_out Caller-supplied buffer to receive the Date header value
 * @param date_cap Capacity of date_out
 *
 * @return 0 on success, negative on error
 */
int vela_https_head_date(const char *host, const char *port, const char *path,
                         char *date_out, size_t date_cap);

/**
 * Streaming response body callback (used by vela_https_post_json_stream).
 *
 * Invoked with raw, already de-chunked body bytes (`len` may be 0; data is not
 * NUL-terminated). Return:
 *   0  — continue reading
 *   >0 — caller got what it needed, stop early (the call still returns the HTTP
 *        status; the connection is NOT reused because the rest was unread)
 *   <0 — abort the request; that negative value is returned to the caller.
 *
 * Runs on the calling thread (blocking TLS read loop), so it must not block.
 */
typedef int (*vela_http_stream_cb_t)(const char *data, size_t len, void *arg);

/**
 * Streaming variant of vela_https_post_json(): sends the POST and hands each
 * response-body fragment to `cb` as it arrives instead of buffering the whole
 * body. Handles both Transfer-Encoding: chunked and Content-Length framing.
 *
 * The global POST lock is held for the whole stream (LLM/TTS serialisation),
 * so a stream can run for seconds without another HTTPS POST interleaving.
 * A mid-stream uplink loss (BT PAN / 4G down) or socket timeout fails the call
 * with a negative VELA_TLS_ERR_* instead of hanging.
 *
 * @param timeout_ms  overall deadline for the whole request+stream; 0 = default.
 * @return HTTP status code on success (even if cb stopped early), negative
 *         VELA_TLS_ERR_* on transport failure.
 */
int vela_https_post_json_stream(
    const char       *host,
    const char       *port,
    const char       *path,
    const vela_header_t *extra_headers,
    const char       *json_body,
    vela_http_stream_cb_t cb,
    void             *cb_arg,
    uint32_t          timeout_ms);

/**
 * RCA-FIX-A（audio_rca2 F1）：带“首字节主动 deadline”的流式 POST。
 *
 * @param first_body_timeout_ms >0 时，在**首个 body 字节到达前**把 socket 读
 *        超时压到该预算；到点直接返回 -ETIMEDOUT，即使服务端完全静默也能在
 *        8–9s 内结束（而非等 20s 流空闲超时/25s 总预算）。
 *        =0 完全等价旧 vela_https_post_json_stream（TTS/ASR/非语音路径不变）。
 * @param timeout_ms 整体 deadline（生成阶段预算），与 first_body 独立。
 */
int vela_https_post_json_stream_ex(
    const char       *host,
    const char       *port,
    const char       *path,
    const vela_header_t *extra_headers,
    const char       *json_body,
    vela_http_stream_cb_t cb,
    void             *cb_arg,
    uint32_t          timeout_ms,
    uint32_t          first_body_timeout_ms);

/**
 * Mark all pooled TLS connections as stale (call when the network uplink
 * goes down: BT PAN / 4G). Cheap and safe from any thread — it only sets a
 * flag; idle connections are freed on the next pool acquisition.
 */
void vela_tls_pool_invalidate(void);

/**
 * Release all TLS connection pool resources.
 * Call during shutdown to free mbedtls contexts.
 */
void vela_tls_pool_cleanup(void);

/**
 * P0-NET 保守预连接：只完成 TCP+TLS 握手并把连接放回池，**不发送任何 HTTP
 * 请求**，因此不改变 keep=false/截断/重试语义。有 POST 在飞时直接跳过；
 * 失败静默返回负值。应由既有线程在空闲时调用（不新增线程）。
 *
 * @return 0 表示池中已有/新建了暖连接；<0 表示跳过或失败（可忽略）。
 */
int vela_tls_preconnect(const char *host, const char *port);

/**
 * P0-NET 读取最近一次连接分段耗时（毫秒）。
 * pool_hit=true 表示最近一次请求复用了池连接（此时 conn/handshake 是更早
 * 一次新建连接的值，仅作参考）。
 */
void vela_tls_get_last_timing(int64_t *conn_ms, int64_t *handshake_ms,
    bool *pool_hit);

/**
 * P0-NET 新建连接序号：调用方在动作前后各取一次，序号变化表示真的新建了
 * 连接（否则是复用池连接 cache，[lat] 里 conn/handshake 应记 0）。
 */
uint64_t vela_tls_conn_seq(void);

#ifdef __cplusplus
}
#endif
