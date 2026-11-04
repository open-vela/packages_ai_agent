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

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#pragma once

/**
 * agent_mem.h — Runtime memory detection, memory pool, and streaming helpers
 * for the AI Agent agent loop.
 */

#include "agent_compat.h"
#include "agent_config.h"
#include "tools/tool_registry.h"
#include <malloc.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Runtime memory status ─────────────────────────────────── */

typedef struct {
    size_t total_heap;
    size_t free_heap;
    size_t largest_block;
} agent_mem_status_t;

/**
 * Query current heap memory status.
 * Uses mallinfo() when available and safe. On QEMU builds where
 * heap validation may assert (CONFIG_DEBUG_MM + tmpfs fallback),
 * returns conservative defaults that don't constrain allocations.
 */
static inline void agent_mem_get_status(agent_mem_status_t* st)
{
    st->total_heap = 128 * 1024 * 1024;
    st->free_heap = 128 * 1024 * 1024;
    st->largest_block = 64 * 1024 * 1024;

#if !defined(CONFIG_DEBUG_MM)
    /* Only call mallinfo when heap debug assertions are disabled,
     * because CONFIG_DEBUG_MM enables strict node validation in
     * mm_foreach that can assert on edge cases (e.g. tmpfs-only boot) */
    struct mallinfo mi = mallinfo();
    if (mi.arena > 0) {
        st->total_heap = mi.arena;
        st->free_heap = mi.fordblks;
        st->largest_block = mi.fordblks;
    }
#endif
}

/**
 * Calculate a safe allocation size based on available memory.
 * Returns a value between min_size and requested, scaled by
 * available free heap. Reserves AGENT_MEM_RESERVE_BYTES for
 * other subsystems.
 */
#define AGENT_MEM_RESERVE_BYTES (32 * 1024)

static inline size_t agent_mem_safe_size(size_t requested, size_t min_size)
{
    agent_mem_status_t st;
    agent_mem_get_status(&st);

    if (st.free_heap <= AGENT_MEM_RESERVE_BYTES)
        return min_size;

    size_t available = st.free_heap - AGENT_MEM_RESERVE_BYTES;
    if (available >= requested)
        return requested;
    if (available >= min_size)
        return available;
    return min_size;
}

/* ── Memory pool for tool output buffers ───────────────────── */

#define AGENT_POOL_SLOTS AGENT_MAX_TOOL_CALLS

typedef struct {
    char* buffers[AGENT_POOL_SLOTS];
    bool in_use[AGENT_POOL_SLOTS];
    size_t buf_size;
    int count;
    pthread_mutex_t lock;
} agent_mem_pool_t;

/**
 * Initialize a memory pool with `count` pre-allocated buffers.
 * Returns OK on success, ERROR if allocation fails.
 */
static inline int agent_pool_init(agent_mem_pool_t* pool, size_t buf_size,
    int count)
{
    if (count > AGENT_POOL_SLOTS)
        count = AGENT_POOL_SLOTS;

    memset(pool, 0, sizeof(*pool));
    pool->buf_size = buf_size;
    pool->count = count;
    pthread_mutex_init(&pool->lock, NULL);

    for (int i = 0; i < count; i++) {
        pool->buffers[i] = calloc(1, buf_size);
        if (!pool->buffers[i]) {
            /* Partial init: free what we got */
            for (int j = 0; j < i; j++)
                free(pool->buffers[j]);
            return ERROR;
        }
        pool->in_use[i] = false;
    }
    return OK;
}

/**
 * Acquire a buffer from the pool. Returns NULL if all slots busy.
 */
static inline char* agent_pool_acquire(agent_mem_pool_t* pool)
{
    char* buf = NULL;
    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < pool->count; i++) {
        if (!pool->in_use[i]) {
            pool->in_use[i] = true;
            pool->buffers[i][0] = '\0';
            buf = pool->buffers[i];
            break;
        }
    }
    pthread_mutex_unlock(&pool->lock);
    return buf;
}

/**
 * Release a buffer back to the pool.
 */
static inline void agent_pool_release(agent_mem_pool_t* pool, char* buf)
{
    if (!buf)
        return;
    pthread_mutex_lock(&pool->lock);
    for (int i = 0; i < pool->count; i++) {
        if (pool->buffers[i] == buf) {
            pool->in_use[i] = false;
            break;
        }
    }
    pthread_mutex_unlock(&pool->lock);
}

/**
 * Destroy the pool and free all buffers.
 */
static inline void agent_pool_destroy(agent_mem_pool_t* pool)
{
    for (int i = 0; i < pool->count; i++) {
        free(pool->buffers[i]);
        pool->buffers[i] = NULL;
    }
    pthread_mutex_destroy(&pool->lock);
}

/* ── Streaming tool output ─────────────────────────────────── */

/**
 * Growable buffer for streaming tool output. Starts small and
 * grows on demand, avoiding large upfront allocations for tools
 * that return small results.
 */
typedef struct {
    char* data;
    size_t len;
    size_t cap;
    size_t max_cap;
} agent_stream_buf_t;

#define AGENT_STREAM_INIT_SIZE 1024
#define AGENT_STREAM_MAX_SIZE (32 * 1024)

static inline int agent_stream_init(agent_stream_buf_t* sb, size_t max_cap)
{
    size_t init = AGENT_STREAM_INIT_SIZE;
    if (init > max_cap)
        init = max_cap;

    sb->data = malloc(init);
    if (!sb->data)
        return ERROR;
    sb->data[0] = '\0';
    sb->len = 0;
    sb->cap = init;
    sb->max_cap = max_cap;
    return OK;
}

/**
 * Append data to the stream buffer, growing as needed.
 * Returns OK on success, ERROR if max_cap would be exceeded.
 */
static inline int agent_stream_append(agent_stream_buf_t* sb,
    const char* chunk, size_t chunk_len)
{
    if (sb->len + chunk_len + 1 > sb->cap) {
        size_t new_cap = sb->cap * 2;
        if (new_cap < sb->len + chunk_len + 1)
            new_cap = sb->len + chunk_len + 1;
        if (new_cap > sb->max_cap)
            new_cap = sb->max_cap;
        if (sb->len + chunk_len + 1 > new_cap)
            return ERROR; /* would exceed max */

        char* new_data = realloc(sb->data, new_cap);
        if (!new_data)
            return ERROR;
        sb->data = new_data;
        sb->cap = new_cap;
    }
    memcpy(sb->data + sb->len, chunk, chunk_len);
    sb->len += chunk_len;
    sb->data[sb->len] = '\0';
    return OK;
}

/**
 * Detach the buffer data (caller takes ownership). Resets the stream.
 */
static inline char* agent_stream_detach(agent_stream_buf_t* sb)
{
    char* p = sb->data;
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
    return p;
}

static inline void agent_stream_free(agent_stream_buf_t* sb)
{
    free(sb->data);
    sb->data = NULL;
    sb->len = 0;
    sb->cap = 0;
}

/**
 * Execute a tool with streaming output. Uses a growable buffer
 * internally, then copies the result to the caller's fixed buffer.
 * This avoids allocating a large fixed buffer upfront for tools
 * that may return small results.
 *
 * Returns the number of bytes written (excluding NUL), or -1 on error.
 *
 * NOTE: tool is executed exactly ONCE. No retry to avoid double
 * side-effects on write/mutating tools.
 */
static inline int agent_tool_exec_streamed(
    const char* name, const char* input_json,
    char* output, size_t output_size)
{
    /* Allocate a buffer sized to output_size so the tool can write its
     * full result without truncation.  We avoid the small-then-retry
     * pattern because retrying would execute the tool twice, which is
     * wrong for write/mutating tools (write_file, cron_add, etc.). */
    size_t buf_size = agent_mem_safe_size(output_size, AGENT_STREAM_INIT_SIZE);
    char* buf = malloc(buf_size);
    if (buf) {
        buf[0] = '\0';
        tool_registry_execute(name, input_json, buf, buf_size);
        size_t result_len = strlen(buf);
        if (result_len >= output_size)
            result_len = output_size - 1;
        memcpy(output, buf, result_len);
        output[result_len] = '\0';
        free(buf);
        return (int)result_len;
    }

    /* OOM fallback: execute directly into caller buffer */
    output[0] = '\0';
    tool_registry_execute(name, input_json, output, output_size);
    return (int)strlen(output);
}

#ifdef __cplusplus
}
#endif
