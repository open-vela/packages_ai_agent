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
        syslog(LOG_ERR, "[compat] task_create '%s' failed: %d "
            "(stack=%d prio=%d)\n", name, r, stack_size, prio);
        free(ta);
        return ERROR;
    }
    pthread_detach(tid);
    return OK;
}

/* ── Cryptographically secure random bytes ─────────────────────────────────── */

#include <fcntl.h>

/**
 * Fill buffer with cryptographically secure random bytes.
 * Uses /dev/urandom (preferred) or /dev/random as fallback.
 *
 * SECURITY: This function will block or fail rather than return
 * predictable data. For TLS entropy, this is the correct behavior.
 *
 * Returns 0 on success, -1 on failure (caller must handle).
 */
static inline int agent_secure_random(void *buf, size_t len)
{
    if (len == 0) return 0;

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) fd = open("/dev/random", O_RDONLY);
    if (fd < 0) return -1;

    unsigned char *p = (unsigned char *)buf;
    size_t remaining = len;

    while (remaining > 0) {
        ssize_t n = read(fd, p, remaining);
        if (n > 0) {
            p += n;
            remaining -= (size_t)n;
        } else if (n == 0) {
            /* EOF on random device — should not happen */
            close(fd);
            return -1;
        } else if (errno != EINTR) {
            /* Real error */
            close(fd);
            return -1;
        }
        /* EINTR: retry */
    }

    close(fd);
    return 0;
}

/* ── Global shutdown ───────────────────────────────────────────────────────── */

/** Request graceful shutdown of all agent threads. */
void agent_request_shutdown(void);

/** Returns true once shutdown has been requested. */
bool agent_shutdown_requested(void);
