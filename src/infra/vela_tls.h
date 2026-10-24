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

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "agent_timing.h"
#include "infra/req_ctl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum size of HTTP response header section we buffer internally */
#define VELA_TLS_HDR_BUF  4096

/* Return codes (negative on error, HTTP status code on success).  The values
 * are shared with the public agent_http.h so a caller that only sees the
 * public header can name them. */
#define VELA_TLS_ERR_CONNECT   AGENT_HTTP_ERR_CONNECT
#define VELA_TLS_ERR_HANDSHAKE AGENT_HTTP_ERR_HANDSHAKE
#define VELA_TLS_ERR_WRITE     AGENT_HTTP_ERR_WRITE
#define VELA_TLS_ERR_READ      AGENT_HTTP_ERR_READ
#define VELA_TLS_ERR_OVERFLOW  AGENT_HTTP_ERR_OVERFLOW
#define VELA_TLS_ERR_TIMEOUT   AGENT_HTTP_ERR_TIMEOUT
#define VELA_TLS_ERR_CANCELLED AGENT_HTTP_ERR_CANCELLED
#define VELA_TLS_ERR_SINK      AGENT_HTTP_ERR_SINK

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
 * Same request, with per-phase timing.
 *
 * @param timing  Optional.  When non-NULL the transport fills in the phases
 *                it can see: pool reuse, fused DNS+TCP, TLS handshake, send,
 *                first response byte and body.  Phases that did not run stay
 *                at AGENT_T_NA.  The caller owns the struct and must reset
 *                it before each attempt.
 *
 * Splitting DNS from TCP is deliberately not attempted: mbedtls_net_connect()
 * performs both in one call, and a second resolver call would change the
 * latency being measured.  dns_tcp_ms is therefore reported as fused.
 */
int vela_https_request_timed(
    const char       *host,
    const char       *port,
    const char       *method,
    const char       *path,
    const vela_header_t *headers,
    const char       *body,
    size_t            body_len,
    char             *resp_buf,
    size_t            resp_cap,
    size_t           *out_body_len,
    agent_llm_timing_t *timing);

/**
 * One HTTPS request bounded by a deadline and a cancel flag.
 *
 * Opens its own connection rather than using the keep-alive pool: the
 * request may carry a large body and the response may be streamed, and a
 * cancelled request must not leave a half-read connection behind for the
 * next caller.  The pool is still used by every other request.
 *
 * @param body   Optional request body source; NULL sends no body.
 * @param sink   Optional response consumer; NULL buffers into resp_buf.
 * @param ctl    Deadline and cancel flag; NULL means neither.
 * @param out_status  Receives the HTTP status, including 4xx and 5xx.
 *
 * @return 0 when a complete HTTP response was read, negative
 *         VELA_TLS_ERR_* otherwise.  On any error the socket is shut down
 *         and the TLS context freed before returning.
 */
int vela_https_request_stream(
    const char       *host,
    const char       *port,
    const char       *method,
    const char       *path,
    const vela_header_t *headers,
    const vela_body_src_t *body,
    const vela_resp_sink_t *sink,
    const vela_req_ctl_t *ctl,
    char             *resp_buf,
    size_t            resp_cap,
    size_t           *out_body_len,
    int              *out_status,
    agent_llm_timing_t *timing);

/* Voice-dedicated keep-alive variant of the streaming entry.  Under
 * CONFIG_AI_AGENT_VOICE_CONN_SLOT a single connection slot is reused
 * sequentially by the voice round's ASR/TTS requests (same thread);
 * every other caller keeps vela_https_request_stream.  A reused
 * connection that fails before any response byte is retried once on a
 * fresh connection.  Without the config this delegates to the plain
 * entry byte-for-byte. */
int vela_https_request_stream_voice(
    const char       *host,
    const char       *port,
    const char       *method,
    const char       *path,
    const vela_header_t *headers,
    const vela_body_src_t *body,
    const vela_resp_sink_t *sink,
    const vela_req_ctl_t *ctl,
    char             *resp_buf,
    size_t            resp_cap,
    size_t           *out_body_len,
    int              *out_status,
    agent_llm_timing_t *timing);

/* Warm the voice slot with a full DNS+TCP+TLS handshake while the user
 * is still speaking, so the ASR right after the button release does
 * not pay it.  No HTTP request is sent.  Without the config it is a
 * no-op.  Returns 0 or a VELA_TLS_ERR_* connect error. */
int vela_https_voice_preconnect(const char *host, const char *port,
                                const vela_req_ctl_t *ctl,
                                agent_llm_timing_t *timing);

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
 * Release all TLS connection pool resources.
 * Call during shutdown to free mbedtls contexts.
 */
void vela_tls_pool_cleanup(void);

#ifdef __cplusplus
}
#endif
