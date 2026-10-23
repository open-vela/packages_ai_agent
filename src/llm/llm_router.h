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
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * LLM Router - Intelligent model routing inspired by ClawRouter
 *
 * Features:
 * - Multi-backend support (up to 4 backends)
 * - Complexity-based routing (simple/medium/complex)
 * - Automatic failover on backend failure
 * - Cost optimization (eco/auto/premium profiles)
 */

/* Maximum number of LLM backends */
#define LLM_ROUTER_MAX_BACKENDS 4

/* Routing profiles (inspired by ClawRouter) */
typedef enum {
    LLM_ROUTE_AUTO = 0, /* Balanced: quality vs cost */
    LLM_ROUTE_ECO, /* Cheapest model */
    LLM_ROUTE_PREMIUM, /* Best quality */
} llm_route_profile_t;

/* Task complexity levels */
typedef enum {
    LLM_COMPLEXITY_SIMPLE = 0, /* Short queries, simple tasks */
    LLM_COMPLEXITY_MEDIUM, /* Normal conversations */
    LLM_COMPLEXITY_COMPLEX, /* Reasoning, coding, analysis */
} llm_complexity_t;

/* Backend configuration */
typedef struct {
    char host[128];
    char path[128];
    char port[8];
    char api_key[128];
    char model[64];
    int priority; /* Lower = higher priority */
    int cost_tier; /* 0=free, 1=cheap, 2=mid, 3=premium */
    bool enabled;
    int fail_count; /* Consecutive failures */

    /* P0 improvements: metrics + backoff (from scout report 2026-03-21) */
    uint32_t last_fail_ts; /* Epoch seconds of last failure */
    uint32_t backoff_until; /* Don't use until this epoch */
    uint32_t total_calls; /* Lifetime call count */
    uint32_t total_failures; /* Lifetime failure count */
    uint32_t avg_latency_ms; /* Exponential moving average */
    uint32_t total_prompt_tokens; /* Cumulative prompt tokens */
    uint32_t total_completion_tokens; /* Cumulative completion tokens */
} llm_backend_t;

/**
 * Initialize the router module.
 * Loads backend configurations from config_store.
 */
int llm_router_init(void);

/**
 * Add or update a backend configuration.
 * @param index Backend index (0 to LLM_ROUTER_MAX_BACKENDS-1)
 * @param backend Backend configuration
 * @return 0 on success, -1 on error
 */
int llm_router_set_backend(int index, const llm_backend_t* backend);

/**
 * Get backend configuration.
 * @param index Backend index
 * @param backend Output buffer
 * @return 0 on success, -1 if not configured
 */
int llm_router_get_backend(int index, llm_backend_t* backend);

/**
 * Set the routing profile.
 * @param profile Routing profile (eco/auto/premium)
 */
void llm_router_set_profile(llm_route_profile_t profile);

/**
 * Get current routing profile.
 */
llm_route_profile_t llm_router_get_profile(void);

/**
 * Estimate task complexity from prompt text.
 * Uses simple heuristics: length, keywords, etc.
 * @param prompt The user prompt
 * @param prompt_len Length of prompt
 * @return Estimated complexity level
 */
llm_complexity_t llm_router_estimate_complexity(const char* prompt, size_t prompt_len);

/**
 * Select the best backend for a task with a specific profile override.
 * Does not change the persistent profile setting.
 * @param complexity Task complexity
 * @param profile Profile to use for this selection
 * @return Backend index, or -1 if no backend available
 */
int llm_router_select_with_profile(llm_complexity_t complexity,
    llm_route_profile_t profile);

/**
 * Select the best backend for a task.
 * Considers: profile, complexity, backend availability, cost.
 * @param complexity Task complexity
 * @return Backend index, or -1 if no backend available
 */
int llm_router_select(llm_complexity_t complexity);

/**
 * Report backend failure (for failover logic).
 * @param index Backend index that failed
 */
void llm_router_report_failure(int index);

/**
 * Report backend success (resets failure count).
 * @param index Backend index that succeeded
 */
void llm_router_report_success(int index);

/**
 * Report request latency for a backend.
 * Updates exponential moving average.
 * @param index Backend index
 * @param latency_ms Request latency in milliseconds
 */
void llm_router_report_latency(int index, uint32_t latency_ms);

/**
 * Report token usage for a backend.
 * @param index Backend index
 * @param prompt_tokens Prompt tokens used
 * @param completion_tokens Completion tokens used
 */
void llm_router_report_tokens(int index, int prompt_tokens,
    int completion_tokens);

/**
 * Apply selected backend to llm_proxy.
 * Sets host/path/port/api_key/model in llm_proxy module.
 * @param index Backend index
 * @return 0 on success, -1 on error
 */
int llm_router_apply(int index);
/* Voice/controlled selection spends no time rewriting stored credentials. */
int llm_router_apply_runtime(int index);

/**
 * Get router status as JSON string.
 * Caller must free the returned string.
 */
char* llm_router_status_json(void);

#ifdef __cplusplus
}
#endif
