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
 * agent_timing.h — Segmented timing for one LLM request attempt.
 *
 * The board path used to report a single number per ReAct step, which made
 * every slow round look like one opaque wait.  This struct records the
 * phases separately so a slow round can be attributed to connection setup,
 * upload, server wait, download or JSON parsing without guessing.
 *
 * Rules that keep the numbers honest:
 *   - Every value is measured on CLOCK_MONOTONIC, so a wall-clock jump
 *     cannot move a phase.  Round elapsed used to be epoch seconds.
 *   - A phase that was not executed is AGENT_T_NA, never zero.
 *   - DNS and TCP are one fused phase here: mbedtls_net_connect() resolves
 *     and connects in one call.  fused_dns_tcp says so; splitting them
 *     would need a second resolver call and would change the measurement.
 *   - ttfb is the first byte of the HTTP response, which is the status
 *     line.  This path is not streamed, so it is not a model first token.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A phase that did not run.  Zero is a real measurement (an instant copy),
 * so it must not double as "not applicable". */

#define AGENT_T_NA (-1)

typedef struct {
    /* Transport phases, filled by vela_tls. */

    int32_t pool_ms; /* pool slot acquisition and reuse check */
    int32_t dns_tcp_ms; /* resolver plus TCP connect, fused */
    int32_t tls_setup_ms; /* RNG seed and TLS config, before the socket */
    int32_t tls_ms; /* TLS handshake */
    int32_t send_ms; /* request headers and body written */
    int32_t ttfb_ms; /* send done to first response byte */
    int32_t body_ms; /* first byte to body complete, headers included */

    /* LLM layer phases. */

    int32_t build_ms; /* request JSON construction and serialization */
    int32_t parse_ms; /* response JSON parse and field extraction */
    int32_t http_total_ms; /* whole transport call */
    int32_t llm_total_ms; /* whole llm_chat_tools call */
    int32_t backoff_ms; /* sleep before this attempt, 0 for the first */

    /* Streaming-request observation.  first_data_ms is the first SSE
     * "data:" line the response sink saw, measured from the moment the
     * request body finished writing; a request without a streaming sink
     * leaves it NA.  It is the model's first visible output, not a tool
     * argument completion.  send_done_mono is the absolute monotonic
     * millisecond of that same send-done moment so upper layers can
     * anchor their own block timestamps to it; 0 means unknown. */

    int32_t first_data_ms; /* send done to first SSE data line */
    uint32_t send_done_mono; /* absolute monotonic ms at send done, 0=unknown */

    int http_rc; /* raw transport return value */
    int http_status; /* parsed HTTP status, 0 when unknown */
    uint32_t req_bytes; /* serialized request size */
    uint32_t resp_bytes; /* HTTP body bytes received */
    uint16_t attempt; /* 1-based attempt within this LLM call */

    bool reused; /* request ran on a pooled connection */
    bool fused_dns_tcp; /* dns_tcp_ms is a combined phase */
    bool proxy; /* request went through the CONNECT proxy */
    bool parse_ok; /* JSON parse and field extraction succeeded */
    bool streamed; /* request ran through a streaming sink */
} agent_llm_timing_t;

/**
 * Monotonic milliseconds.  Wraps after about 49 days, which no board run
 * reaches; elapsed math below stays correct across the wrap anyway.
 */

static inline uint32_t agent_mono_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return (uint32_t)((uint64_t)ts.tv_sec * 1000u
        + (uint64_t)ts.tv_nsec / 1000000u);
}

static inline int32_t agent_elapsed_ms(uint32_t t0, uint32_t t1)
{
    uint32_t d = t1 - t0;

    /* A single request never runs for 24 days; a larger difference can only
     * mean the caller mixed clock bases.  Report it as unmeasured. */

    return d > 0x7fffffffu ? AGENT_T_NA : (int32_t)d;
}

/* Elapsed since *t0, then move *t0 to now.  Used for back-to-back phases. */

static inline int32_t agent_lap_ms(uint32_t* t0)
{
    uint32_t now = agent_mono_ms();
    int32_t ms = agent_elapsed_ms(*t0, now);

    *t0 = now;
    return ms;
}

/** Reset every field to "not measured" before one attempt. */
void agent_timing_reset(agent_llm_timing_t* t);

/**
 * Mark the transport phases as not applicable.  The proxy path cannot see
 * inside its tunnel, so it calls this and only reports the total.
 */
void agent_timing_no_transport(agent_llm_timing_t* t);

/**
 * Format one attempt as a single syslog line body, without the "[trace:...]"
 * prefix or a trailing newline.
 *
 * The request body, headers, API key and tool output are never included;
 * only sizes and timings are.  Returns the number of characters written,
 * or -1 when the buffer is too small.  A truncated line would drop fields
 * and read as a complete record, so this fails instead of truncating.
 */
int agent_timing_format_http(char* buf, size_t cap, int iteration,
    const agent_llm_timing_t* t);

#ifdef __cplusplus
}
#endif
