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

/**
 * agent_http.h — a bounded, cancellable POST to the configured endpoint.
 *
 * The chat path builds one JSON string and reads one reply, which is fine
 * for a few KB.  The voice path cannot: a 15 s recording is 480 KB of PCM
 * and 640 KB once base64-encoded, and a synthesis reply can be 400 KB.  This
 * API lets the caller produce the request body and consume the reply in
 * pieces, under one deadline and one cancel flag.
 *
 * The endpoint, credentials and TLS setup come from the agent configuration,
 * so a caller never handles the API key.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest single blocking I/O slice.  Bounds how late a cancel is noticed. */

#define AGENT_HTTP_IO_SLICE_MS 500u

/* Return codes.  Zero means a complete HTTP response was read, whatever its
 * status; every negative value means no usable response. */

#define AGENT_HTTP_ERR_CONNECT   -1
#define AGENT_HTTP_ERR_HANDSHAKE -2
#define AGENT_HTTP_ERR_WRITE     -3
#define AGENT_HTTP_ERR_READ      -4
#define AGENT_HTTP_ERR_OVERFLOW  -5
#define AGENT_HTTP_ERR_TIMEOUT   -6
#define AGENT_HTTP_ERR_CANCELLED -7
#define AGENT_HTTP_ERR_SINK      -8

/**
 * One deadline and one cancel flag for a whole request.
 *
 * deadline_ms is CLOCK_MONOTONIC milliseconds; 0 means "no deadline".  The
 * subtraction is unsigned, so a monotonic wrap does not extend a request.
 * cancel points to an int written by another thread; it is polled between
 * I/O slices.
 */
typedef struct {
    uint32_t deadline_ms;
    const volatile int *cancel;
} vela_req_ctl_t;

/**
 * Request body produced in pieces.  len is the exact byte count that read()
 * will produce; it becomes Content-Length, so the server never has to guess.
 * read() returns the bytes written, 0 at end, or negative on error.
 */
typedef struct {
    size_t len;
    int (*read)(void *ctx, char *buf, size_t cap);
    void *ctx;
} vela_body_src_t;

/**
 * Response body consumer.  When set, bytes are handed over as they arrive.
 * write() returns 0 to continue, negative to abort.
 */
typedef struct {
    int (*write)(void *ctx, const char *data, size_t len);
    void *ctx;
} vela_resp_sink_t;

/* ── deadline helpers ──────────────────────────────────────────── */

static inline int32_t agent_http_remaining_ms(uint32_t deadline_ms,
    uint32_t now_ms)
{
    int32_t d;

    if (deadline_ms == 0) {
        return -1;
    }

    d = (int32_t)(deadline_ms - now_ms);
    return d > 0 ? d : 0;
}

static inline int32_t agent_http_budget_ms(const vela_req_ctl_t *ctl,
    uint32_t now_ms)
{
    if (ctl == NULL) {
        return -1;
    }

    return agent_http_remaining_ms(ctl->deadline_ms, now_ms);
}

static inline bool agent_http_cancelled(const vela_req_ctl_t *ctl)
{
    return ctl != NULL && ctl->cancel != NULL && *ctl->cancel != 0;
}

static inline bool agent_http_stop(const vela_req_ctl_t *ctl, uint32_t now_ms)
{
    if (agent_http_cancelled(ctl)) {
        return true;
    }

    return agent_http_budget_ms(ctl, now_ms) == 0;
}

static inline uint32_t agent_http_io_timeout_ms(const vela_req_ctl_t *ctl,
    uint32_t now_ms, uint32_t cap_ms)
{
    int32_t left = agent_http_budget_ms(ctl, now_ms);

    if (left < 0) {
        return cap_ms;
    }

    if (left == 0) {
        return 0;
    }

    return ((uint32_t)left < cap_ms) ? (uint32_t)left : cap_ms;
}

/* ── request ───────────────────────────────────────────────────── */

/**
 * POST to the configured LLM endpoint.
 *
 * @param post_data  Body as one string; used when body is NULL.
 * @param body       Optional streaming body; takes precedence.
 * @param sink       Optional streaming reply; NULL buffers into resp_buf.
 * @param out_status Receives the HTTP status, including 4xx and 5xx.
 *
 * @return 0 when a complete HTTP response was read, negative on a transport
 *         failure.  The connection is closed before returning in every case.
 */
int agent_http_post_stream(const char *post_data,
    const vela_body_src_t *body,
    const vela_resp_sink_t *sink,
    char *resp_buf, size_t resp_cap,
    size_t *out_body_len, int *out_status,
    const vela_req_ctl_t *ctl);

/* Explicit transport diagnostic.  Sends a fixed invalid test credential,
 * never reads configured credentials, discards the response body, and never
 * changes the product endpoint.  Not registered as an Agent tool. */
int agent_http_probe(const char *host, const char *port, const char *path,
    const vela_req_ctl_t *ctl, size_t *out_body_len, int *out_status);

#ifdef __cplusplus
}
#endif
