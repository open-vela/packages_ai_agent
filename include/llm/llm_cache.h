/*
 * Copyright (C) 2025 Xiaomi Corporation
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

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Simple LLM response cache — 8-slot LRU with TTL.
 *
 * Key: djb2 hash of first 64 chars of prompt.
 * Value: cached response text (strdup'd).
 * TTL: 300 seconds.
 */

#define LLM_CACHE_SLOTS 8
#define LLM_CACHE_TTL_SEC 300
#define LLM_CACHE_KEY_LEN 64

/**
 * Initialize the cache (call once at startup).
 */
void llm_cache_init(void);

/**
 * Look up a cached response for the given prompt.
 * @param prompt  User prompt text
 * @param prompt_len  Length of prompt
 * @return Cached response (caller must free), or NULL on miss
 */
char* llm_cache_get(const char* prompt, size_t prompt_len);

/**
 * Store a response in the cache with token info.
 * @param prompt  User prompt text
 * @param prompt_len  Length of prompt
 * @param response  Response text to cache
 */
/* Drop the entry for this prompt.  A caller with a freshness requirement
 * (the voice round must run its tool every time) uses this before asking so
 * a previous model answer cannot skip the tool. */
void llm_cache_forget(const char* prompt, size_t prompt_len);

void llm_cache_put(const char* prompt, size_t prompt_len,
    const char* response);

/**
 * Store token count alongside cached response.
 * Call after llm_cache_put for the same prompt.
 * @param prompt  User prompt text
 * @param prompt_len  Length of prompt
 * @param total_tokens  Token count to associate
 */
void llm_cache_put_tokens(const char* prompt, size_t prompt_len,
    int total_tokens);

/**
 * Get cumulative tokens saved by cache hits.
 */
uint32_t llm_cache_tokens_saved(void);

/**
 * Get total cache hit count.
 */
uint32_t llm_cache_hit_count(void);

/**
 * Free all cached responses (call during shutdown).
 */
void llm_cache_cleanup(void);

#ifdef __cplusplus
}
#endif
