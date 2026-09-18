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

#include "remote/remote_link_mqtt.h"
#include "remote/remote_codec.h"

#include "agent_compat.h"

#include <mqtt.h>

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "remote";

/* Room for one full-size link message plus MQTT framing. */
#define REMOTE_MQTT_BUFFER_SIZE (REMOTE_MESSAGE_MAX + 512)
#define REMOTE_MQTT_TOPIC_MAX 160
#define REMOTE_MQTT_KEEPALIVE 30
#define REMOTE_MQTT_SYNC_INTERVAL_MS 100
#define REMOTE_MQTT_RETRY_DELAY_S 1
#define REMOTE_MQTT_CONNECT_TIMEOUT_MS 5000

/* 18 random bytes rendered as hex. Long enough that a stale bridge session
 * cannot be replayed against a new connection. */
#define REMOTE_MQTT_NONCE_BYTES 18

struct remote_mqtt_s {
    struct mqtt_client mqtt;
    struct remote_session_s* session;
    struct remote_transport_s transport;
    char host[REMOTE_MQTT_HOST_MAX + 1];
    char port[REMOTE_MQTT_PORT_MAX + 1];
    char username[REMOTE_MQTT_USER_MAX + 1];
    char password[REMOTE_MQTT_PASSWORD_MAX + 1];
    char device_id[REMOTE_MQTT_DEVICE_ID_MAX + 1];
    char to_bridge[REMOTE_MQTT_TOPIC_MAX];
    char to_device[REMOTE_MQTT_TOPIC_MAX];
    uint8_t sendbuf[REMOTE_MQTT_BUFFER_SIZE] __attribute__((aligned(sizeof(uintptr_t))));
    uint8_t recvbuf[REMOTE_MQTT_BUFFER_SIZE];
};

/* How long remote_link_mqtt_stop() waits for the loop to leave. A connect
 * attempt sitting in poll() can outlast this; the restart guard below is what
 * makes that safe rather than racy. */
#define REMOTE_MQTT_STOP_WAIT_S 2

/* One remote session per device, so a single static instance avoids ~9 KiB of
 * heap churn across reconnects. Because the instance is shared, only one loop
 * may own it at a time — see g_loop_active. */
static struct remote_mqtt_s g_mqtt;
static volatile bool g_running;

/* Ownership of the static transport above. A stop() followed immediately by a
 * start() (as an NSH reconfigure does) must not put two loops on it. */
static pthread_mutex_t g_state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_state_cond = PTHREAD_COND_INITIALIZER;
static bool g_loop_active;

static void release_loop(void)
{
    pthread_mutex_lock(&g_state_lock);
    g_loop_active = false;
    pthread_cond_broadcast(&g_state_cond);
    pthread_mutex_unlock(&g_state_lock);
}

static uint32_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ── Transport operation ───────────────────────────────────────────── */

/* Called by the session with its lock released, so taking MQTT-C's client
 * mutex here cannot invert lock order against the inbound path. */
static bool send_json(void* context, const char* json, size_t length)
{
    struct remote_mqtt_s* transport = context;

    if (transport == NULL || length == 0 || length > REMOTE_MESSAGE_MAX) {
        return false;
    }

    return mqtt_publish(&transport->mqtt, transport->to_bridge, json, length,
               MQTT_PUBLISH_QOS_0)
        == MQTT_OK;
}

static const struct remote_transport_ops_s g_transport_ops = {
    .send_json = send_json,
};

/* ── Inbound ───────────────────────────────────────────────────────── */

/* MQTT-C runs this while holding its own client mutex. Everything below only
 * takes the session lock, never publishes. */
static void publish_callback(void** state, struct mqtt_response_publish* published)
{
    struct remote_mqtt_s* transport = (state != NULL) ? *state : NULL;

    if (transport == NULL || transport->session == NULL) {
        return;
    }

    /* Ignore anything that is not our exact inbound topic: a wildcard
     * subscription elsewhere on the broker must not feed this session. */
    if (published->topic_name_size != strlen(transport->to_device) || memcmp(published->topic_name, transport->to_device, published->topic_name_size) != 0) {
        return;
    }

    remote_codec_receive(transport->session,
        published->application_message,
        published->application_message_size, now_ms());
}

/* ── Setup helpers ─────────────────────────────────────────────────── */

static bool copy_config(const struct remote_mqtt_config_s* config,
    struct remote_mqtt_s* transport)
{
    const char* prefix = (config->topic_prefix != NULL && config->topic_prefix[0] != '\0')
        ? config->topic_prefix
        : REMOTE_MQTT_TOPIC_PREFIX_DEFAULT;
    int written;

    if (config->host == NULL || config->host[0] == '\0' || config->device_id == NULL || config->device_id[0] == '\0' || strlen(config->host) > REMOTE_MQTT_HOST_MAX || strlen(config->device_id) > REMOTE_MQTT_DEVICE_ID_MAX || strlen(prefix) > REMOTE_MQTT_TOPIC_PREFIX_MAX) {
        return false;
    }

    /* A wildcard in the prefix would subscribe this device to other devices'
     * traffic, so reject it rather than sanitising it. */
    if (strchr(prefix, '+') != NULL || strchr(prefix, '#') != NULL || strchr(config->device_id, '+') != NULL || strchr(config->device_id, '#') != NULL || strchr(config->device_id, '/') != NULL) {
        return false;
    }

    if ((config->port != NULL && strlen(config->port) > REMOTE_MQTT_PORT_MAX) || (config->username != NULL && strlen(config->username) > REMOTE_MQTT_USER_MAX) || (config->password != NULL && strlen(config->password) > REMOTE_MQTT_PASSWORD_MAX)) {
        return false;
    }

    strcpy(transport->host, config->host);
    strcpy(transport->device_id, config->device_id);
    snprintf(transport->port, sizeof(transport->port), "%s",
        (config->port != NULL && config->port[0] != '\0') ? config->port : "1883");
    snprintf(transport->username, sizeof(transport->username), "%s",
        config->username != NULL ? config->username : "");
    snprintf(transport->password, sizeof(transport->password), "%s",
        config->password != NULL ? config->password : "");

    written = snprintf(transport->to_bridge, sizeof(transport->to_bridge),
        "%s/%s/device-to-bridge", prefix, transport->device_id);
    if (written < 0 || (size_t)written >= sizeof(transport->to_bridge)) {
        return false;
    }

    written = snprintf(transport->to_device, sizeof(transport->to_device),
        "%s/%s/bridge-to-device", prefix, transport->device_id);
    return written > 0 && (size_t)written < sizeof(transport->to_device);
}

/* MQTT-C requires a non-blocking socket. */
static int connect_socket(const char* host, const char* port)
{
    struct addrinfo hints;
    struct addrinfo* result;
    struct addrinfo* item;
    int fd = -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &result) != 0) {
        syslog(LOG_ERR, "[%s] DNS resolve failed: %s\n", TAG, host);
        return -1;
    }

    for (item = result; item != NULL; item = item->ai_next) {
        int flags;

        fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
        if (fd < 0) {
            continue;
        }

        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }

        if (connect(fd, item->ai_addr, item->ai_addrlen) == 0) {
            break;
        }

        if (errno == EINPROGRESS) {
            struct pollfd entry;

            entry.fd = fd;
            entry.events = POLLOUT;
            entry.revents = 0;
            if (poll(&entry, 1, REMOTE_MQTT_CONNECT_TIMEOUT_MS) > 0) {
                int error = 0;
                socklen_t error_size = sizeof(error);

                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_size) == 0 && error == 0) {
                    break;
                }
            }
        }

        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

static bool make_nonce(char* out, size_t out_size)
{
    unsigned char bytes[REMOTE_MQTT_NONCE_BYTES];
    size_t index;

    if (out_size < sizeof(bytes) * 2 + 1) {
        return false;
    }

    /* A predictable nonce would let a stale bridge session be replayed, so a
     * failure here aborts the connection instead of falling back to time(). */
    if (agent_secure_random(bytes, sizeof(bytes)) != 0) {
        syslog(LOG_ERR, "[%s] secure random unavailable, cannot open session\n", TAG);
        return false;
    }

    for (index = 0; index < sizeof(bytes); index++) {
        snprintf(&out[index * 2], 3, "%02x", bytes[index]);
    }

    return true;
}

/* ── Run loop ──────────────────────────────────────────────────────── */

int remote_link_mqtt_run(const struct remote_mqtt_config_s* config,
    struct remote_session_s* session)
{
    struct remote_mqtt_s* transport = &g_mqtt;
    char client_id[REMOTE_MQTT_DEVICE_ID_MAX + 24];
    char nonce[REMOTE_NONCE_MAX + 1];
    int written;

    if (config == NULL || session == NULL) {
        return ERROR;
    }

    /* Claim the shared transport before anything writes to it. copy_config()
     * targets g_mqtt, so without this even a rejected start would scribble on
     * the config of a loop that is still running. */
    pthread_mutex_lock(&g_state_lock);
    if (g_loop_active) {
        pthread_mutex_unlock(&g_state_lock);
        syslog(LOG_WARNING,
            "[%s] a transport loop is still active, not starting another\n", TAG);
        return ERROR;
    }
    g_loop_active = true;
    pthread_mutex_unlock(&g_state_lock);

    if (!copy_config(config, transport)) {
        syslog(LOG_ERR, "[%s] invalid MQTT configuration\n", TAG);
        release_loop();
        return ERROR;
    }

    written = snprintf(client_id, sizeof(client_id), "remote-ctrl-%s",
        transport->device_id);
    if (written < 0 || (size_t)written >= sizeof(client_id)) {
        release_loop();
        return ERROR;
    }

    transport->session = session;
    transport->transport.ops = &g_transport_ops;
    transport->transport.context = transport;
    g_running = true;

    while (g_running) {
        int fd = connect_socket(transport->host, transport->port);

        if (fd < 0) {
            syslog(LOG_WARNING, "[%s] broker connect failed: %s:%s\n", TAG,
                transport->host, transport->port);
            sleep(REMOTE_MQTT_RETRY_DELAY_S);
            continue;
        }

        /* Every reconnect is a new session: fresh nonce, fresh handshake, and
         * Offline until a matching snapshot arrives. This is why the transport
         * does not use mqtt_init_reconnect — the boundary has to be visible. */
        if (!make_nonce(nonce, sizeof(nonce))) {
            close(fd);
            sleep(REMOTE_MQTT_RETRY_DELAY_S);
            continue;
        }

        remote_session_new_connection(session, nonce);

        mqtt_init(&transport->mqtt, fd, transport->sendbuf,
            sizeof(transport->sendbuf), transport->recvbuf,
            sizeof(transport->recvbuf), publish_callback);
        transport->mqtt.publish_response_callback_state = transport;

        if (mqtt_connect(&transport->mqtt, client_id, NULL, NULL, 0,
                transport->username[0] ? transport->username : NULL,
                transport->password[0] ? transport->password : NULL,
                MQTT_CONNECT_CLEAN_SESSION, REMOTE_MQTT_KEEPALIVE)
                != MQTT_OK
            || mqtt_subscribe(&transport->mqtt, transport->to_device, 0) != MQTT_OK) {
            syslog(LOG_WARNING, "[%s] MQTT connect or subscribe failed\n", TAG);
            close(fd);
            sleep(REMOTE_MQTT_RETRY_DELAY_S);
            continue;
        }

        /* The transport must be active before hello: the handshake goes out
         * through the same path as every other outbound message. */
        remote_session_set_active_transport(session, &transport->transport);

        if (!remote_session_send_hello(session)) {
            syslog(LOG_WARNING, "[%s] hello publish failed\n", TAG);
            remote_session_set_active_transport(session, NULL);
            close(fd);
            sleep(REMOTE_MQTT_RETRY_DELAY_S);
            continue;
        }

        syslog(LOG_INFO, "[%s] connected as %s via %s:%s\n", TAG,
            transport->device_id, transport->host, transport->port);

        while (g_running && mqtt_sync(&transport->mqtt) == MQTT_OK && transport->mqtt.error == MQTT_OK) {
            /* Independent of TCP health: a bridge that stops sending snapshots
             * drops the session back to Offline. */
            remote_session_timeout(session, now_ms());
            usleep(REMOTE_MQTT_SYNC_INTERVAL_MS * 1000);
        }

        syslog(LOG_INFO, "[%s] disconnected\n", TAG);
        remote_session_link_down(session);
        remote_session_set_active_transport(session, NULL);
        close(fd);

        if (g_running) {
            sleep(REMOTE_MQTT_RETRY_DELAY_S);
        }
    }

    remote_session_link_down(session);
    remote_session_set_active_transport(session, NULL);
    syslog(LOG_INFO, "[%s] transport loop exited\n", TAG);
    release_loop();
    return OK;
}

void remote_link_mqtt_stop(void)
{
    struct timespec deadline;

    g_running = false;

    pthread_mutex_lock(&g_state_lock);
    if (!g_loop_active) {
        pthread_mutex_unlock(&g_state_lock);
        return;
    }

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += REMOTE_MQTT_STOP_WAIT_S;

    while (g_loop_active) {
        if (pthread_cond_timedwait(&g_state_cond, &g_state_lock, &deadline) == ETIMEDOUT) {
            /* Most likely parked in a connect attempt. Returning here is safe
             * because a restart is refused while the loop still owns the
             * transport. */
            syslog(LOG_WARNING, "[%s] transport loop still winding down\n", TAG);
            break;
        }
    }

    pthread_mutex_unlock(&g_state_lock);
}
