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
 * agent_trace.h — Structured execution tracing for AI Agent Agent
 *
 * Provides run_id-based structured logging so that each agent
 * execution can be traced end-to-end via syslog. Every log line
 * within a single ask/task is tagged with the same run_id.
 *
 * Trace status codes:
 *   0 = running
 *   1 = ok (completed successfully)
 *   2 = fail (LLM or tool error)
 *   3 = timeout (iteration limit reached)
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Trace status codes */
#define AGENT_TRACE_RUNNING 0
#define AGENT_TRACE_OK 1
#define AGENT_TRACE_FAIL 2
#define AGENT_TRACE_TIMEOUT 3

typedef struct {
    char run_id[17]; /* 16 hex chars + NUL */
    char chat_id[64];
    char channel[16];
    uint32_t start_ts; /* monotonic seconds */
    int iteration; /* current iteration number */
    int backend_idx; /* current router backend index */
    uint32_t total_latency_ms; /* cumulative LLM latency */
    int total_tool_calls; /* cumulative tool call count */
    int status; /* AGENT_TRACE_* */
} agent_trace_t;

/**
 * Begin a new trace. Generates a unique run_id and logs the start.
 */
void agent_trace_begin(agent_trace_t* t, const char* chat_id,
    const char* channel);

/**
 * Log one iteration step (after each LLM call + tool execution).
 */
void agent_trace_step(agent_trace_t* t, int iteration,
    const char* tool_name, uint32_t latency_ms,
    int llm_ok);

/**
 * End the trace and log the summary.
 */
void agent_trace_end(agent_trace_t* t, int status);

#ifdef __cplusplus
}
#endif
