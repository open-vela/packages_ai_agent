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

#include "agent_config.h"
#include "channels/mqtt_channel.h"
#include "agent_compat.h"
#include "infra/config_store.h"
#include "core/message_bus.h"
#include "cJSON.h"

#include <mqtt.h>

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/select.h>

static const char *TAG = "mqtt";

/* Bounded timeout for the TCP connect in tcp_connect(). A blocking
 * connect() can stall for tens of seconds on an unreachable broker;
 * on_reconnect() runs while holding s_client.mutex, so a long stall there
 * also blocks the outbound dispatch task (mqtt_publish waits on the same
 * mutex). Keep it short enough to avoid stalling dispatch. */
#define MQTT_TCP_CONNECT_TIMEOUT_MS 5000

/* ── Configuration ─────────────────────────────────────────── */

static char s_broker_host[128];
static int  s_broker_port;
static char s_client_id[64];
static char s_topic_in[128];
static char s_topic_out[128];
static char s_username[64];
static char s_password[128];

/* ── MQTT-C state ──────────────────────────────────────────── */

static struct mqtt_client s_client;
static uint8_t s_sendbuf[AGENT_MQTT_BUF_SIZE] __attribute__((aligned(4)));
static uint8_t s_recvbuf[AGENT_MQTT_BUF_SIZE];
static int     s_sockfd = -1;
static volatile bool s_running;
static volatile bool s_sync_thread_exited = true;
static bool s_client_inited = false;

/* ── TCP connect helper ────────────────────────────────────── */

static int tcp_connect(const char *host, int port)
{
    char port_str[8];
    int fd;
    int flags;

    snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    struct addrinfo *res;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        syslog(LOG_ERR, "[%s] DNS resolve failed: %s\n", TAG, host);
        return -1;
    }

    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);

    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    /* Non-blocking connect with a bounded timeout (see the comment on
     * MQTT_TCP_CONNECT_TIMEOUT_MS for why this matters). */
    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0
        && errno != EINPROGRESS) {
        syslog(LOG_ERR, "[%s] TCP connect failed: %s:%d (%s)\n",
               TAG, host, port, strerror(errno));
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);

    fd_set wfds;
    FD_ZERO(&wfds);
    FD_SET(fd, &wfds);
    struct timeval tv = {
        .tv_sec = MQTT_TCP_CONNECT_TIMEOUT_MS / 1000,
        .tv_usec = (MQTT_TCP_CONNECT_TIMEOUT_MS % 1000) * 1000
    };

    if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
        syslog(LOG_ERR, "[%s] TCP connect timeout: %s:%d\n",
               TAG, host, port);
        close(fd);
        return -1;
    }

    int sock_err = 0;
    socklen_t sock_errlen = sizeof(sock_err);

    getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &sock_errlen);
    if (sock_err != 0) {
        syslog(LOG_ERR, "[%s] TCP connect failed: %s:%d (%s)\n",
               TAG, host, port, strerror(sock_err));
        close(fd);
        return -1;
    }

    /* Keep the socket NON-BLOCKING for subsequent send/recv.
     *
     * This is the actual fix for the reminder being silent: MQTT-C's
     * mqtt_pal_recvall()/mqtt_pal_sendall() are written for non-blocking
     * sockets (EAGAIN == "nothing to do"), and mqtt_sync's usleep() paces the
     * loop. A *blocking* recv() here blocks while holding client->mutex inside
     * __mqtt_recv(), which both starves mqtt_publish() on the outbound
     * dispatch thread AND stops __mqtt_send() from ever flushing the queue
     * (the sync thread never reaches __mqtt_send while stuck in recv). On a
     * flaky WiFi (half-dead TCP, no FIN/RST) SO_RCVTIMEO does not even fire,
     * so the block is effectively forever. Non-blocking recv() always returns
     * EAGAIN immediately. */
    /* (leave fd in O_NONBLOCK from the connect above) */

    /* Keep SO_RCVTIMEO as a fallback in case the stack ignores O_NONBLOCK on
     * recv — worst case it bounds the block to 1s. */
    struct timeval tv_rcv;

    tv_rcv.tv_sec = 1;
    tv_rcv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv_rcv, sizeof(tv_rcv));

    syslog(LOG_INFO, "[%s] TCP connected to %s:%d (fd=%d, non-blocking)\n",
           TAG, host, port, fd);
    return fd;
}

/* ── Publish callback (incoming messages from broker) ──────── */

static void on_publish(void **state, struct mqtt_response_publish *pub)
{
    (void)state;

    if (pub->application_message_size == 0) {
        return;
    }

    /* NUL-terminate the payload (MQTT-C does not guarantee it) */
    size_t len = pub->application_message_size;
    char *payload = malloc(len + 1);

    if (!payload) {
        return;
    }

    memcpy(payload, pub->application_message, len);
    payload[len] = '\0';

    syslog(LOG_INFO, "[%s] Recv [%.*s]: %.64s\n",
           TAG, (int)pub->topic_name_size,
           (const char *)pub->topic_name, payload);

    /* Parse JSON: {"type":"message","content":"...","chat_id":"..."} */
    cJSON *root = cJSON_Parse(payload);

    if (!root) {
        /* Plain text fallback — treat entire payload as content */
        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_MQTT, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, "mqtt_default", sizeof(msg.chat_id) - 1);
        msg.content = payload; /* bus takes ownership on success */
        if (message_bus_push_inbound(&msg) != OK) {
            free(payload);
        }
        return;
    }

    cJSON *content_item = cJSON_GetObjectItem(root, "content");
    cJSON *chat_id_item = cJSON_GetObjectItem(root, "chat_id");

    if (cJSON_IsString(content_item) && content_item->valuestring[0]) {
        agent_msg_t msg;

        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, AGENT_CHAN_MQTT, sizeof(msg.channel) - 1);

        if (cJSON_IsString(chat_id_item) && chat_id_item->valuestring[0]) {
            strncpy(msg.chat_id, chat_id_item->valuestring,
                    sizeof(msg.chat_id) - 1);
        } else {
            strncpy(msg.chat_id, "mqtt_default", sizeof(msg.chat_id) - 1);
        }

        msg.content = strdup(content_item->valuestring);
        if (msg.content) {
            if (message_bus_push_inbound(&msg) != OK) {
                free(msg.content);
            }
        }
    }

    cJSON_Delete(root);
    free(payload);
}

/* ── Reconnect callback ────────────────────────────────────── */

static void on_reconnect(struct mqtt_client *client, void **state)
{
    (void)state;

    /* Close old socket if any */
    if (s_sockfd >= 0) {
        close(s_sockfd);
        s_sockfd = -1;
    }

    syslog(LOG_INFO, "[%s] Reconnecting to %s:%d...\n",
           TAG, s_broker_host, s_broker_port);

    s_sockfd = tcp_connect(s_broker_host, s_broker_port);
    if (s_sockfd < 0) {
        syslog(LOG_ERR, "[%s] Reconnect TCP failed\n", TAG);
        /* Reinit with invalid fd so MQTT-C stays in error state
         * and will call reconnect_callback again on next mqtt_sync. */
        mqtt_reinit(client, -1,
                    s_sendbuf, sizeof(s_sendbuf),
                    s_recvbuf, sizeof(s_recvbuf));
        return;
    }

    mqtt_reinit(client, s_sockfd,
                s_sendbuf, sizeof(s_sendbuf),
                s_recvbuf, sizeof(s_recvbuf));

    uint8_t flags = MQTT_CONNECT_CLEAN_SESSION;
    const char *user = s_username[0] ? s_username : NULL;
    const char *pass = s_password[0] ? s_password : NULL;

    enum MQTTErrors err = mqtt_connect(client, s_client_id,
                                       NULL, NULL, 0,
                                       user, pass, flags,
                                       AGENT_MQTT_KEEPALIVE);
    if (err != MQTT_OK) {
        syslog(LOG_ERR, "[%s] mqtt_connect failed: %d\n", TAG, (int)err);
        return;
    }

    /* Subscribe to inbound topic */
    mqtt_subscribe(client, s_topic_in, 1);
    syslog(LOG_INFO, "[%s] Subscribed to %s\n", TAG, s_topic_in);
}

/* ── Sync thread (calls mqtt_sync periodically) ───────────── */

static void *mqtt_sync_thread(void *arg)
{
    (void)arg;

    syslog(LOG_INFO, "[%s] Sync thread started\n", TAG);

    while (s_running) {
        enum MQTTErrors err = mqtt_sync(&s_client);

        if (err != MQTT_OK) {
            syslog(LOG_WARNING, "[%s] mqtt_sync error: %d\n", TAG, (int)err);
            /* MQTT-C will call reconnect_callback automatically */
        }

        usleep(AGENT_MQTT_SYNC_INTERVAL_MS * 1000);
    }

    s_sync_thread_exited = true;
    syslog(LOG_INFO, "[%s] Sync thread exited\n", TAG);
    return NULL;
}

/* ── Parse broker URL "host:port" ──────────────────────────── */

static void parse_broker(const char *broker, char *host, size_t host_sz,
                         int *port)
{
    const char *colon = strrchr(broker, ':');

    if (colon && colon != broker) {
        size_t hlen = (size_t)(colon - broker);

        if (hlen >= host_sz) {
            hlen = host_sz - 1;
        }

        memcpy(host, broker, hlen);
        host[hlen] = '\0';
        *port = atoi(colon + 1);
    } else {
        strncpy(host, broker, host_sz - 1);
        host[host_sz - 1] = '\0';
        *port = AGENT_MQTT_DEFAULT_PORT;
    }

    if (*port <= 0 || *port > 65535) {
        *port = AGENT_MQTT_DEFAULT_PORT;
    }
}

/* ── Public API ────────────────────────────────────────────── */

int mqtt_channel_init(void)
{
    char broker_buf[192];

    memset(broker_buf, 0, sizeof(broker_buf));
    memset(s_broker_host, 0, sizeof(s_broker_host));
    memset(s_client_id, 0, sizeof(s_client_id));
    memset(s_topic_in, 0, sizeof(s_topic_in));
    memset(s_topic_out, 0, sizeof(s_topic_out));
    memset(s_username, 0, sizeof(s_username));
    memset(s_password, 0, sizeof(s_password));
    s_broker_port = AGENT_MQTT_DEFAULT_PORT;

    /* Read config from config store */
    claw_config_get(AGENT_CFG_KEY_MQTT_BROKER,
                    broker_buf, sizeof(broker_buf));
    claw_config_get(AGENT_CFG_KEY_MQTT_CLIENT_ID,
                    s_client_id, sizeof(s_client_id));
    claw_config_get(AGENT_CFG_KEY_MQTT_TOPIC_IN,
                    s_topic_in, sizeof(s_topic_in));
    claw_config_get(AGENT_CFG_KEY_MQTT_TOPIC_OUT,
                    s_topic_out, sizeof(s_topic_out));
    claw_config_get(AGENT_CFG_KEY_MQTT_USERNAME,
                    s_username, sizeof(s_username));
    claw_config_get(AGENT_CFG_KEY_MQTT_PASSWORD,
                    s_password, sizeof(s_password));

    /* Parse broker host:port */
    if (broker_buf[0]) {
        parse_broker(broker_buf, s_broker_host,
                     sizeof(s_broker_host), &s_broker_port);
    }

    /* Defaults */
    if (!s_client_id[0]) {
        snprintf(s_client_id, sizeof(s_client_id),
                 "agent-%s", AGENT_NODE_ID);
    }

    if (!s_topic_in[0]) {
        strncpy(s_topic_in, AGENT_MQTT_TOPIC_IN_DEFAULT,
                sizeof(s_topic_in) - 1);
    }

    if (!s_topic_out[0]) {
        strncpy(s_topic_out, AGENT_MQTT_TOPIC_OUT_DEFAULT,
                sizeof(s_topic_out) - 1);
    }

    syslog(LOG_INFO, "[%s] MQTT channel initialized (broker=%s:%d)\n",
           TAG, s_broker_host[0] ? s_broker_host : "(not set)",
           s_broker_port);
    return OK;
}

int mqtt_channel_start(void)
{
    if (!s_broker_host[0]) {
        syslog(LOG_INFO, "[%s] No mqtt_broker configured, MQTT disabled\n",
               TAG);
        return OK;
    }

    /* Idempotent: if a sync thread is already running, don't start another.
     * Two threads sharing one s_client + the same client_id will CONNECT
     * twice and the broker keeps kicking one off → endless reconnect loop. */
    if (s_running) {
        syslog(LOG_INFO, "[%s] MQTT already running, skip duplicate start\n",
               TAG);
        return OK;
    }

    /* Initialize the MQTT-C client exactly once. mqtt_init_reconnect()
     * re-initializes s_client.mutex every call; re-initing a mutex that
     * another thread (outbound dispatch via mqtt_publish, or a still-stuck
     * old sync thread) may be blocked on is undefined behavior and can leave
     * that thread permanently blocked. On re-start we soft-reset instead. */
    if (!s_client_inited) {
        mqtt_init_reconnect(&s_client, on_reconnect, NULL, on_publish);
        s_client_inited = true;
    } else {
        mqtt_reinit(&s_client, -1,
                    s_sendbuf, sizeof(s_sendbuf),
                    s_recvbuf, sizeof(s_recvbuf));
        /* mqtt_reinit leaves reconnect_callback/publish_response_callback
         * intact from the first init; only reset the error so the first
         * mqtt_sync triggers on_reconnect. */
        s_client.error = MQTT_ERROR_INITIAL_RECONNECT;
    }

    s_running = true;
    s_sync_thread_exited = false;

    int ret = agent_task_create(mqtt_sync_thread, "mqtt_sync",
                                   AGENT_MQTT_STACK, NULL,
                                   AGENT_MQTT_PRIO);
    if (ret != OK) {
        syslog(LOG_ERR, "[%s] Failed to create sync thread\n", TAG);
        s_running = false;
        s_sync_thread_exited = true;
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] MQTT channel started → %s:%d\n",
           TAG, s_broker_host, s_broker_port);
    return OK;
}

int mqtt_channel_send(const char *chat_id, const char *text)
{
    if (!s_running || s_sockfd < 0 || !chat_id || !text) {
        return ERROR;
    }

    /* Build JSON: {"type":"response","content":"...","chat_id":"..."} */
    cJSON *resp = cJSON_CreateObject();

    cJSON_AddStringToObject(resp, "type", "response");
    cJSON_AddStringToObject(resp, "content", text);
    cJSON_AddStringToObject(resp, "chat_id", chat_id);

    char *json = cJSON_PrintUnformatted(resp);

    cJSON_Delete(resp);
    if (!json) {
        return ERROR;
    }

    enum MQTTErrors err = mqtt_publish(&s_client, s_topic_out,
                                       json, strlen(json),
                                       MQTT_PUBLISH_QOS_1);
    if (err != MQTT_OK) {
        syslog(LOG_ERR, "[%s] Publish failed: %d\n", TAG, (int)err);
        free(json);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Published to %s (%d bytes)\n",
           TAG, s_topic_out, (int)strlen(json));
    free(json);
    return OK;
}

void mqtt_channel_stop(void)
{
    if (!s_running) {
        return;
    }

    s_running = false;

    /* Close socket first to unblock any blocking recv in sync thread */
    if (s_sockfd >= 0) {
        close(s_sockfd);
        s_sockfd = -1;
    }

    /* Wait for the sync thread to actually exit before returning. Without
     * this, a fast stop→start (e.g. `set_mqtt`) leaves the old thread alive:
     * start() flips s_running back to true and creates a second thread, and
     * the two threads CONNECT with the same client_id → the broker keeps
     * kicking one off → endless reconnect loop (MQTT_ERROR_SOCKET_ERROR). */
    for (int i = 0; i < 20 && !s_sync_thread_exited; i++) {
        usleep(100 * 1000);
    }

    syslog(LOG_INFO, "[%s] MQTT channel stopped\n", TAG);
}
