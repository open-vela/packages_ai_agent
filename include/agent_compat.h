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
/*
 * agent_compat.h — Vela/NuttX platform abstraction
 *
 * Provides common includes and lightweight helpers used across AI Agent.
 * Error handling uses NuttX native OK/ERROR + errno convention.
 */

/* Expose strcasestr(), memmem(), and other POSIX extensions on glibc-based
 * host builds (e.g. unit-test compilation on Linux).  NuttX provides these
 * unconditionally, so this define is a no-op there. */
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h> /* struct timeval */
#include <sys/types.h> /* OK, ERROR */
#include <syslog.h> /* LOG_INFO / LOG_ERR / LOG_WARNING / LOG_DEBUG */
#include <time.h>
#include <unistd.h>

/* ── Task helper ───────────────────────────────────────────────────────────── */

typedef struct {
    void* (*func)(void*);
    void* arg;
    int stack_size;
} _agent_task_args_t;

static inline void* _agent_task_shim(void* p)
{
    _agent_task_args_t* ta = (_agent_task_args_t*)p;
    void* (*f)(void*) = ta->func;
    void* a = ta->arg;
    free(ta);
    return f(a);
}

/**
 * Create a detached POSIX thread.
 * Returns OK (0) on success, ERROR (-1) on failure.
 */
static inline int agent_task_create(void* (*func)(void*), const char* name,
    int stack_size, void* arg, int prio)
{
    (void)name;

    _agent_task_args_t* ta = malloc(sizeof(_agent_task_args_t));
    if (!ta)
        return ERROR;
    ta->func = func;
    ta->arg = arg;
    ta->stack_size = stack_size;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, (size_t)(stack_size < 4096 ? 4096 : stack_size));

    /* Set thread priority via SCHED_FIFO (NuttX supports this) */
    struct sched_param sp;
    sp.sched_priority = prio;
    pthread_attr_setschedparam(&attr, &sp);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);

    pthread_t tid;
    int r = pthread_create(&tid, &attr, _agent_task_shim, ta);
    pthread_attr_destroy(&attr);
    if (r != 0) {
        free(ta);
        return ERROR;
    }
    pthread_detach(tid);
    return OK;
}

/* ── Cryptographically secure random bytes ─────────────────────────────────── */

#include <fcntl.h>

/**
 * Fill buffer with secure random bytes.
 *
 * WORKAROUND (SF32LB52): the /dev/urandom path crashes on this board
 * because a pre-existing heap overflow corrupts the inode list and
 * _inode_compare dereferences a broken node (hardfault observed in
 * cmd_net_test -> TLS entropy init). arc4random_buf needs no inode
 * lookup and is already used by the SF32LB52 H4 driver. TODO: locate
 * the heap overflow root cause and restore /dev/urandom entropy.
 *
 * Returns 0 on success, -1 on failure (caller must handle).
 */
static inline int agent_secure_random(void *buf, size_t len)
{
    if (len == 0) return 0;

    arc4random_buf(buf, len);
    return 0;
}

/* ── Global shutdown ───────────────────────────────────────────────────────── */

/** Request graceful shutdown of all agent threads. */
void agent_request_shutdown(void);

/** Returns true once shutdown has been requested. */
bool agent_shutdown_requested(void);
