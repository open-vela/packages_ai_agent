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

#include "core/agent_trace.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <syslog.h>

static const char* TAG = "trace";

static const char* status_str(int status)
{
    switch (status) {
    case AGENT_TRACE_RUNNING:
        return "running";
    case AGENT_TRACE_OK:
        return "ok";
    case AGENT_TRACE_FAIL:
        return "fail";
    case AGENT_TRACE_TIMEOUT:
        return "timeout";
    default:
        return "unknown";
    }
}

void agent_trace_begin(agent_trace_t* t, const char* chat_id,
    const char* channel)
{
    struct timespec tv = {0};

    memset(t, 0, sizeof(*t));
    clock_gettime(CLOCK_MONOTONIC, &tv);
    t->start_ts = (uint32_t)tv.tv_sec;

    /* Generate run_id: lower 32 bits of time + rand for uniqueness */
    snprintf(t->run_id, sizeof(t->run_id), "%08x%08x",
        (unsigned)(tv.tv_sec & 0xFFFFFFFF),
        (unsigned)(rand() & 0xFFFFFFFF));

    if (chat_id) {
        strncpy(t->chat_id, chat_id, sizeof(t->chat_id) - 1);
    }
    if (channel) {
        strncpy(t->channel, channel, sizeof(t->channel) - 1);
    }

    t->status = AGENT_TRACE_RUNNING;
    t->backend_idx = -1;

    syslog(LOG_INFO,
        "[%s:%s] BEGIN chat=%s chan=%s",
        TAG, t->run_id, t->chat_id, t->channel);
}

void agent_trace_step(agent_trace_t* t, int iteration,
    const char* tool_name, uint32_t latency_ms,
    int llm_ok)
{
    t->iteration = iteration;
    t->total_latency_ms += latency_ms;
    if (tool_name && tool_name[0]) {
        t->total_tool_calls++;
    }

    syslog(LOG_INFO,
        "[%s:%s] iter=%d tool=%s latency=%ums llm=%s backend=%d",
        TAG, t->run_id, iteration,
        (tool_name && tool_name[0]) ? tool_name : "(none)",
        (unsigned)latency_ms,
        llm_ok ? "ok" : "fail",
        t->backend_idx);
}

void agent_trace_end(agent_trace_t* t, int status)
{
    struct timespec tv = {0};

    clock_gettime(CLOCK_MONOTONIC, &tv);
    t->status = status;

    uint32_t elapsed = (uint32_t)tv.tv_sec - t->start_ts;

    syslog(LOG_INFO,
        "[%s:%s] END status=%s iters=%d tools=%d "
        "llm_ms=%u elapsed=%us backend=%d",
        TAG, t->run_id, status_str(status),
        t->iteration + 1, t->total_tool_calls,
        (unsigned)t->total_latency_ms, (unsigned)elapsed,
        t->backend_idx);
}
