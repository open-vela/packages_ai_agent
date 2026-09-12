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

#include "llm/llm_cache.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

static const char* TAG = "llm_cache";

typedef struct {
    uint32_t hash;
    char* response;
    uint32_t ts; /* epoch seconds when stored */
    uint32_t hits;
    int tokens; /* total_tokens when cached */
} llm_cache_slot_t;

static llm_cache_slot_t s_slots[LLM_CACHE_SLOTS];
static pthread_mutex_t s_cache_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_tokens_saved;
static uint32_t s_total_hits;

/* djb2 hash over the FULL string. Hash the whole message, not just a fixed
 * prefix: kid_buddy sends "[SYSTEM]…\n[USER]\n<query>", so the meaningful
 * part (the query) is at the END. Hashing only the first LLM_CACHE_KEY_LEN
 * bytes made every query under one role collide on the same cached reply. */

static uint32_t djb2_hash(const char* str, size_t len)
{
    uint32_t h = 5381;

    for (size_t i = 0; i < len; i++) {
        h = ((h << 5) + h) + (unsigned char)str[i];
    }
    return h;
}

void llm_cache_init(void)
{
    pthread_mutex_lock(&s_cache_lock);
    for (int i = 0; i < LLM_CACHE_SLOTS; i++) {
        if (s_slots[i].response) {
            free(s_slots[i].response);
            s_slots[i].response = NULL;
        }
        s_slots[i].hash = 0;
        s_slots[i].ts = 0;
        s_slots[i].hits = 0;
        s_slots[i].tokens = 0;
    }
    s_tokens_saved = 0;
    s_total_hits = 0;
    pthread_mutex_unlock(&s_cache_lock);
    syslog(LOG_INFO, "[%s] Cache initialized (%d slots, TTL %ds)\n",
        TAG, LLM_CACHE_SLOTS, LLM_CACHE_TTL_SEC);
}

char* llm_cache_get(const char* prompt, size_t prompt_len)
{
    if (!prompt || prompt_len == 0) {
        return NULL;
    }

    uint32_t h = djb2_hash(prompt, prompt_len);
    uint32_t now = (uint32_t)time(NULL);
    char* result = NULL;

    pthread_mutex_lock(&s_cache_lock);
    for (int i = 0; i < LLM_CACHE_SLOTS; i++) {
        llm_cache_slot_t* s = &s_slots[i];
        if (s->hash == h && s->response) {
            /* Check TTL */
            if (now - s->ts > LLM_CACHE_TTL_SEC) {
                free(s->response);
                s->response = NULL;
                s->hash = 0;
                break;
            }
            result = strdup(s->response);
            if (result) {
                s->hits++;
                s_total_hits++;
                if (s->tokens > 0) {
                    s_tokens_saved += (uint32_t)s->tokens;
                }
            }
            break;
        }
    }
    pthread_mutex_unlock(&s_cache_lock);
    return result;
}

void llm_cache_put(const char* prompt, size_t prompt_len,
    const char* response)
{
    if (!prompt || prompt_len == 0 || !response) {
        return;
    }

    uint32_t h = djb2_hash(prompt, prompt_len);
    uint32_t now = (uint32_t)time(NULL);

    pthread_mutex_lock(&s_cache_lock);

    /* Find: existing slot with same hash, or LRU (oldest ts) */
    int target = 0;
    uint32_t oldest_ts = UINT32_MAX;

    for (int i = 0; i < LLM_CACHE_SLOTS; i++) {
        if (s_slots[i].hash == h) {
            target = i;
            break;
        }
        if (s_slots[i].ts < oldest_ts) {
            oldest_ts = s_slots[i].ts;
            target = i;
        }
    }

    llm_cache_slot_t* s = &s_slots[target];
    if (s->response) {
        free(s->response);
        s->response = NULL;
    }

    s->response = strdup(response);
    if (!s->response) {
        pthread_mutex_unlock(&s_cache_lock);
        return;
    }
    s->hash = h;
    s->ts = now;
    s->hits = 0;

    pthread_mutex_unlock(&s_cache_lock);
}

void llm_cache_put_tokens(const char* prompt, size_t prompt_len,
    int total_tokens)
{
    if (!prompt || prompt_len == 0 || total_tokens <= 0) {
        return;
    }

    uint32_t h = djb2_hash(prompt, prompt_len);

    pthread_mutex_lock(&s_cache_lock);
    for (int i = 0; i < LLM_CACHE_SLOTS; i++) {
        if (s_slots[i].hash == h && s_slots[i].response) {
            s_slots[i].tokens = total_tokens;
            break;
        }
    }
    pthread_mutex_unlock(&s_cache_lock);
}

uint32_t llm_cache_tokens_saved(void)
{
    uint32_t val;
    pthread_mutex_lock(&s_cache_lock);
    val = s_tokens_saved;
    pthread_mutex_unlock(&s_cache_lock);
    return val;
}

uint32_t llm_cache_hit_count(void)
{
    uint32_t val;
    pthread_mutex_lock(&s_cache_lock);
    val = s_total_hits;
    pthread_mutex_unlock(&s_cache_lock);
    return val;
}

void llm_cache_cleanup(void)
{
    pthread_mutex_lock(&s_cache_lock);
    for (int i = 0; i < LLM_CACHE_SLOTS; i++) {
        free(s_slots[i].response);
        s_slots[i].response = NULL;
    }
    pthread_mutex_unlock(&s_cache_lock);
}
