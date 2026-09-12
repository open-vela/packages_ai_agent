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
 * Streaming response callback. Invoked with decoded response-body bytes as
 * they arrive from the socket (chunked transfer-encoding is decoded before
 * the callback is invoked). Return 0 to keep reading, non-zero to abort the
 * read early.
 */
typedef int (*vela_stream_cb_t)(const char *data, size_t len, void *ctx);

/**
 * POST JSON and stream the response body to a callback instead of buffering
 * the whole body. Used for LLM streaming (SSE). Same request semantics as
 * vela_https_post_json(), but always opens a fresh connection (no pooling)
 * because a streamed response stays open for the whole generation.
 *
 * @return HTTP status code (200, etc.) on success, negative VELA_TLS_ERR_*
 *         on failure.
 */
int vela_https_post_json_stream(const char *host, const char *port,
    const char *path, const vela_header_t *extra_headers,
    const char *json_body, vela_stream_cb_t cb, void *ctx);

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
