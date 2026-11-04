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

#include "core/message_bus.h"
#include "agent_config.h"
#include "agent_compat.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static const char *TAG = "bus";

/* -- Private Types -------------------------------------------------------- */

typedef struct {
    agent_msg_t  items[AGENT_BUS_QUEUE_LEN];
    int             head;
    int             tail;
    int             count;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} msg_queue_t;

/* -- Private Data --------------------------------------------------------- */

static msg_queue_t g_inbound_queue;
static msg_queue_t g_outbound_queue;
static bool g_initialized = false;
static volatile bool g_bus_shutdown = false;

/* -- Private Functions ---------------------------------------------------- */

static void msg_queue_init(msg_queue_t *queue)
{
    memset(queue, 0, sizeof(*queue));
    pthread_mutex_init(&queue->lock, NULL);
    pthread_cond_init(&queue->not_empty, NULL);
    pthread_cond_init(&queue->not_full, NULL);
}

static void msg_queue_destroy(msg_queue_t *queue)
{
    pthread_mutex_destroy(&queue->lock);
    pthread_cond_destroy(&queue->not_empty);
    pthread_cond_destroy(&queue->not_full);
    memset(queue, 0, sizeof(*queue));
}

static int msg_queue_push(msg_queue_t *queue, const agent_msg_t *msg,
                          uint32_t timeout_ms)
{
    struct timespec ts;

    pthread_mutex_lock(&queue->lock);

    if (queue->count >= AGENT_BUS_QUEUE_LEN) {
        if (timeout_ms == 0) {
            pthread_mutex_unlock(&queue->lock);
            return ERROR;
        }

        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(timeout_ms / 1000);
        ts.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        /* Wait for space */
        while (queue->count >= AGENT_BUS_QUEUE_LEN) {
            if (pthread_cond_timedwait(&queue->not_full, &queue->lock, &ts) != 0) {
                pthread_mutex_unlock(&queue->lock);
                syslog(LOG_WARNING, "[%s] Queue full, dropping message\n", TAG);
                return ERROR;
            }
        }
    }

    queue->items[queue->tail] = *msg;
    queue->tail = (queue->tail + 1) % AGENT_BUS_QUEUE_LEN;
    queue->count++;

    pthread_cond_signal(&queue->not_empty);
    pthread_mutex_unlock(&queue->lock);
    return OK;
}

static int msg_queue_pop(msg_queue_t *queue, agent_msg_t *msg,
                         uint32_t timeout_ms)
{
    struct timespec ts;

    pthread_mutex_lock(&queue->lock);

    while (queue->count == 0) {
        if (g_bus_shutdown) {
            pthread_mutex_unlock(&queue->lock);
            return ERROR;
        }
        if (timeout_ms == UINT32_MAX) {
            pthread_cond_wait(&queue->not_empty, &queue->lock);
        } else {
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += (time_t)(timeout_ms / 1000);
            ts.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec++;
                ts.tv_nsec -= 1000000000L;
            }

            if (pthread_cond_timedwait(&queue->not_empty, &queue->lock, &ts) != 0) {
                pthread_mutex_unlock(&queue->lock);
                return ERROR;
            }
        }
    }

    *msg = queue->items[queue->head];
    queue->head = (queue->head + 1) % AGENT_BUS_QUEUE_LEN;
    queue->count--;

    pthread_cond_signal(&queue->not_full);
    pthread_mutex_unlock(&queue->lock);
    return OK;
}

/* -- Public Functions ----------------------------------------------------- */

void message_bus_msg_free(agent_msg_t *msg)
{
    if (msg == NULL) {
        return;
    }

    if (msg->content != NULL) {
        free(msg->content);
        msg->content = NULL;
    }

    if (msg->image_b64 != NULL) {
        free(msg->image_b64);
        msg->image_b64 = NULL;
    }
}

int message_bus_init(void)
{
    if (g_initialized) {
        return OK;
    }

    msg_queue_init(&g_inbound_queue);
    msg_queue_init(&g_outbound_queue);
    g_bus_shutdown = false;
    g_initialized = true;

    syslog(LOG_INFO, "[%s] Message bus initialized (depth %d)\n",
           TAG, AGENT_BUS_QUEUE_LEN);
    return OK;
}

void message_bus_destroy(void)
{
    if (!g_initialized) {
        return;
    }

    msg_queue_destroy(&g_inbound_queue);
    msg_queue_destroy(&g_outbound_queue);
    g_initialized = false;

    syslog(LOG_INFO, "[%s] Message bus destroyed\n", TAG);
}

void message_bus_wakeup(void)
{
    g_bus_shutdown = true;

    pthread_mutex_lock(&g_inbound_queue.lock);
    pthread_cond_broadcast(&g_inbound_queue.not_empty);
    pthread_mutex_unlock(&g_inbound_queue.lock);

    pthread_mutex_lock(&g_outbound_queue.lock);
    pthread_cond_broadcast(&g_outbound_queue.not_empty);
    pthread_mutex_unlock(&g_outbound_queue.lock);
}

int message_bus_push_inbound(const agent_msg_t *msg)
{
    if (msg == NULL) {
        return ERROR;
    }

    return msg_queue_push(&g_inbound_queue, msg, AGENT_BUS_PUSH_TIMEOUT_MS);
}

int message_bus_try_push_inbound(const agent_msg_t *msg)
{
    if (msg == NULL) {
        return ERROR;
    }

    return msg_queue_push(&g_inbound_queue, msg, 0);
}

int message_bus_pop_inbound(agent_msg_t *msg, uint32_t timeout_ms)
{
    if (msg == NULL) {
        return ERROR;
    }

    return msg_queue_pop(&g_inbound_queue, msg, timeout_ms);
}

int message_bus_push_outbound(const agent_msg_t *msg)
{
    if (msg == NULL) {
        return ERROR;
    }

    return msg_queue_push(&g_outbound_queue, msg, AGENT_BUS_PUSH_TIMEOUT_MS);
}

int message_bus_pop_outbound(agent_msg_t *msg, uint32_t timeout_ms)
{
    if (msg == NULL) {
        return ERROR;
    }

    return msg_queue_pop(&g_outbound_queue, msg, timeout_ms);
}
