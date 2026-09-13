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

/**
 * velaclaw_quickapp_bridge.c - Cross-process bridge between QuickApp feature
 * and the AI Agent process using POSIX message queues.
 *
 * Protocol:
 *   Message payload = "chat_id\ncontent" (null-terminated)
 *
 * Queues:
 *   /velaclaw_qapp_in   - QuickApp writes requests, Agent reads
 *   /velaclaw_qapp_out  - Agent writes replies, QuickApp reads
 */

#include "velaclaw_quickapp_bridge.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

/* -- Mqueue configuration (must match agent_main.c) ----------- */
#define VELACLAW_MQ_QAPP_IN   "/velaclaw_qapp_in"
#define VELACLAW_MQ_QAPP_OUT  "/velaclaw_qapp_out"
#define VELACLAW_MQ_MSG_SIZE  4096
#define VELACLAW_MQ_MAX_MSGS  4

#define BRIDGE_TAG "[velaclaw_bridge] "

/* -- Reply callback state ------------------------------------- */
static velaclaw_quickapp_reply_cb s_reply_cb;
static void *s_reply_userdata;
static pthread_t s_recv_thread;
static volatile int s_running;

/* -- Receiver thread: listens for replies from agent process -- */
static void *bridge_recv_thread(void *arg)
{
    (void)arg;

    struct mq_attr attr = {
        .mq_maxmsg  = VELACLAW_MQ_MAX_MSGS,
        .mq_msgsize = VELACLAW_MQ_MSG_SIZE,
    };

    mqd_t mq = mq_open(VELACLAW_MQ_QAPP_OUT, O_RDONLY | O_CREAT, 0666, &attr);
    if (mq == (mqd_t)-1) {
        syslog(LOG_ERR, BRIDGE_TAG "recv: mq_open(%s) failed: %d\n",
               VELACLAW_MQ_QAPP_OUT, errno);
        return NULL;
    }

    char *buf = (char *)malloc(VELACLAW_MQ_MSG_SIZE);
    if (!buf) {
        syslog(LOG_ERR, BRIDGE_TAG "recv: malloc failed\n");
        mq_close(mq);
        return NULL;
    }

    syslog(LOG_INFO, BRIDGE_TAG "recv thread started\n");

    while (s_running) {
        ssize_t n = mq_receive(mq, buf, VELACLAW_MQ_MSG_SIZE, NULL);
        if (n < 0) {
            if (errno == EINTR) continue;
            syslog(LOG_ERR, BRIDGE_TAG "recv: mq_receive failed: %d\n", errno);
            break;
        }

        /* Ensure null-termination */
        if (n >= (ssize_t)VELACLAW_MQ_MSG_SIZE) {
            n = VELACLAW_MQ_MSG_SIZE - 1;
        }
        buf[n] = '\0';

        /* Reply format: "chat_id\ncontent" */
        char *sep = strchr(buf, '\n');
        if (!sep) {
            syslog(LOG_WARNING, BRIDGE_TAG "recv: malformed reply\n");
            continue;
        }
        *sep = '\0';
        const char *chat_id = buf;
        const char *content = sep + 1;

        syslog(LOG_INFO, BRIDGE_TAG "recv: chat_id=%s content_len=%d\n",
               chat_id, (int)strlen(content));

        if (s_reply_cb) {
            s_reply_cb(chat_id, content, NULL, NULL, s_reply_userdata);
        }
    }

    free(buf);
    mq_close(mq);
    syslog(LOG_INFO, BRIDGE_TAG "recv thread exiting\n");
    return NULL;
}

/* -- Public API ----------------------------------------------- */
void velaclaw_quickapp_bridge_register(velaclaw_quickapp_reply_cb cb,
                                       void *userdata)
{
    s_reply_cb = cb;
    s_reply_userdata = userdata;

    if (!s_running) {
        s_running = 1;

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 4096);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

        int ret = pthread_create(&s_recv_thread, &attr, bridge_recv_thread, NULL);
        pthread_attr_destroy(&attr);

        if (ret != 0) {
            syslog(LOG_ERR, BRIDGE_TAG "failed to create recv thread: %d\n", ret);
            s_running = 0;
        }
    }
}

void velaclaw_quickapp_bridge_unregister(void)
{
    s_reply_cb = NULL;
    s_reply_userdata = NULL;
    s_running = 0;
}

int velaclaw_quickapp_bridge_ask(const char *chat_id, const char *query)
{
    if (!query || !query[0]) {
        return -EINVAL;
    }
    if (!chat_id || !chat_id[0]) {
        return -EINVAL;
    }

    struct mq_attr attr = {
        .mq_maxmsg  = VELACLAW_MQ_MAX_MSGS,
        .mq_msgsize = VELACLAW_MQ_MSG_SIZE,
    };

    mqd_t mq = mq_open(VELACLAW_MQ_QAPP_IN, O_WRONLY | O_CREAT, 0666, &attr);
    if (mq == (mqd_t)-1) {
        syslog(LOG_ERR, BRIDGE_TAG "ask: mq_open(%s) failed: %d\n",
               VELACLAW_MQ_QAPP_IN, errno);
        return -ENOTCONN;
    }

    /* Build payload: "chat_id\nquery" */
    char *buf = (char *)malloc(VELACLAW_MQ_MSG_SIZE);
    if (!buf) {
        mq_close(mq);
        return -ENOMEM;
    }

    int len = snprintf(buf, VELACLAW_MQ_MSG_SIZE, "%s\n%s", chat_id, query);
    if (len >= (int)VELACLAW_MQ_MSG_SIZE) {
        len = VELACLAW_MQ_MSG_SIZE - 1;
        buf[len] = '\0';
    }

    int ret = 0;
    if (mq_send(mq, buf, len + 1, 0) != 0) {
        syslog(LOG_ERR, BRIDGE_TAG "ask: mq_send failed: %d\n", errno);
        ret = -errno;
    } else {
        syslog(LOG_INFO, BRIDGE_TAG "ask: sent chat_id=%s query_len=%d\n",
               chat_id, (int)strlen(query));
    }

    free(buf);
    mq_close(mq);
    return ret;
}

/* -- Utility functions ---------------------------------------- */
int velaclaw_quickapp_bridge_get_status(char *output, size_t output_size)
{
    if (!output || output_size == 0) {
        return -EINVAL;
    }
    snprintf(output, output_size,
             "{\"ok\":true,\"bridge\":\"mqueue\",\"running\":%s}",
             s_running ? "true" : "false");
    return 0;
}

int velaclaw_quickapp_bridge_call_tool(const char *app_id,
                                       const char *tool,
                                       const char *args_json,
                                       char *output,
                                       size_t output_size)
{
    (void)app_id;
    (void)tool;
    (void)args_json;

    if (!output || output_size == 0) {
        return -EINVAL;
    }
    snprintf(output, output_size,
             "{\"ok\":false,\"code\":\"not_implemented\","
             "\"message\":\"tool calling not supported via mqueue bridge\"}");
    return -ENOSYS;
}
