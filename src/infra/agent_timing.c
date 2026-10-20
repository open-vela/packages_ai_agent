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

#include "infra/agent_timing.h"

#include <stdio.h>
#include <string.h>

static const char* na_or_ms(char* buf, size_t cap, int32_t ms)
{
    if (ms == AGENT_T_NA) {
        snprintf(buf, cap, "na");
    } else {
        snprintf(buf, cap, "%ld", (long)ms);
    }

    return buf;
}

void agent_timing_reset(agent_llm_timing_t* t)
{
    memset(t, 0, sizeof(*t));

    t->pool_ms = AGENT_T_NA;
    t->dns_tcp_ms = AGENT_T_NA;
    t->tls_setup_ms = AGENT_T_NA;
    t->tls_ms = AGENT_T_NA;
    t->send_ms = AGENT_T_NA;
    t->ttfb_ms = AGENT_T_NA;
    t->body_ms = AGENT_T_NA;
    t->build_ms = AGENT_T_NA;
    t->parse_ms = AGENT_T_NA;
    t->http_total_ms = AGENT_T_NA;
    t->llm_total_ms = AGENT_T_NA;
    t->backoff_ms = 0;
    t->http_rc = 0;
    t->http_status = 0;
    t->req_bytes = 0;
    t->attempt = 1;

    /* Fused is a property of how the phase is measured, not of whether it
     * ran, so it is true even before the transport fills anything in.  The
     * formatter only prints the label next to a real value. */

    t->fused_dns_tcp = true;
}

void agent_timing_no_transport(agent_llm_timing_t* t)
{
    t->pool_ms = AGENT_T_NA;
    t->dns_tcp_ms = AGENT_T_NA;
    t->tls_setup_ms = AGENT_T_NA;
    t->tls_ms = AGENT_T_NA;
    t->send_ms = AGENT_T_NA;
    t->ttfb_ms = AGENT_T_NA;
    t->body_ms = AGENT_T_NA;
    t->fused_dns_tcp = true;
    t->reused = false;
}

/* One format string, used twice: once to measure, once to write.  Keeping a
 * single copy means the length check can never describe a different line
 * than the one logged. */

#define TIMING_HTTP_FMT \
    "iter=%d attempt=%u reused=%u proxy=%u dns_tcp=%s%s setup=%s tls=%s " \
    "send=%s ttfb=%s body=%s pool=%s build=%s parse=%s " \
    "http=%s backoff=%s total=%s req=%lu rc=%d status=%d parse_ok=%u"

#define TIMING_HTTP_ARGS(t, iteration, pool, dns, setup, tls, send, ttfb, \
    body, build, parse, http, total, backoff) \
    iteration, (unsigned)(t)->attempt, (t)->reused ? 1u : 0u, \
        (t)->proxy ? 1u : 0u, \
        na_or_ms((dns), sizeof(dns), (t)->dns_tcp_ms), \
        (t)->fused_dns_tcp ? "(fused)" : "", \
        na_or_ms((setup), sizeof(setup), (t)->tls_setup_ms), \
        na_or_ms((tls), sizeof(tls), (t)->tls_ms), \
        na_or_ms((send), sizeof(send), (t)->send_ms), \
        na_or_ms((ttfb), sizeof(ttfb), (t)->ttfb_ms), \
        na_or_ms((body), sizeof(body), (t)->body_ms), \
        na_or_ms((pool), sizeof(pool), (t)->pool_ms), \
        na_or_ms((build), sizeof(build), (t)->build_ms), \
        na_or_ms((parse), sizeof(parse), (t)->parse_ms), \
        na_or_ms((http), sizeof(http), (t)->http_total_ms), \
        na_or_ms((backoff), sizeof(backoff), (t)->backoff_ms), \
        na_or_ms((total), sizeof(total), (t)->llm_total_ms), \
        (unsigned long)(t)->req_bytes, (t)->http_rc, (t)->http_status, \
        (t)->parse_ok ? 1u : 0u

int agent_timing_format_http(char* buf, size_t cap, int iteration,
    const agent_llm_timing_t* t)
{
    char pool[16], dns[16], setup[16], tls[16], send[16], ttfb[16];
    char body[16];
    char build[16], parse[16], http[16], total[16], backoff[16];
    int n;

    n = snprintf(NULL, 0, TIMING_HTTP_FMT,
        TIMING_HTTP_ARGS(t, iteration, pool, dns, setup, tls, send, ttfb,
            body, build, parse, http, total, backoff));

    if (n < 0 || (size_t)n >= cap) {
        return -1;
    }

    snprintf(buf, cap, TIMING_HTTP_FMT,
        TIMING_HTTP_ARGS(t, iteration, pool, dns, setup, tls, send, ttfb,
            body, build, parse, http, total, backoff));

    return n;
}
