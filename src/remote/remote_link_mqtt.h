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

/* MQTT transport for the remote-control link.
 *
 * Deliberately does not reuse mqtt_channel.c: that channel hands reconnection
 * to MQTT-C via mqtt_init_reconnect, but this link must mint a fresh
 * connection nonce and redo the hello handshake on every reconnect, so the
 * reconnect boundary has to be visible here.
 */

#pragma once

#include "remote/remote_session.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_MQTT_HOST_MAX 127
#define REMOTE_MQTT_PORT_MAX 5
#define REMOTE_MQTT_USER_MAX 63
#define REMOTE_MQTT_PASSWORD_MAX 127
#define REMOTE_MQTT_DEVICE_ID_MAX 32
#define REMOTE_MQTT_TOPIC_PREFIX_MAX 32

/* Wire-compatible default with the PC bridge's --topic-prefix. */
#define REMOTE_MQTT_TOPIC_PREFIX_DEFAULT "deskmate"

struct remote_mqtt_config_s {
    const char* host;
    const char* port;
    const char* username;
    const char* password;
    const char* device_id;
    const char* topic_prefix; /* NULL selects the default */
};

/* Runs the connect / handshake / sync / reconnect loop until
 * remote_link_mqtt_stop(). Blocks, so call it from a dedicated thread.
 *
 * Returns ERROR on invalid configuration, ERROR if a previous loop is still
 * winding down, and OK after a requested stop. The transport keeps its buffers
 * in one static instance, so refusing the overlap is what stops two loops from
 * sharing them. */
int remote_link_mqtt_run(const struct remote_mqtt_config_s* config,
    struct remote_session_s* session);

/* Asks the loop to exit and waits briefly for it to do so. Safe to call from
 * another thread.
 *
 * The wait is bounded: a connect attempt already inside poll() can outlast it.
 * When that happens this returns anyway and an immediate restart is refused by
 * remote_link_mqtt_run() rather than allowed to race. */
void remote_link_mqtt_stop(void);

#ifdef __cplusplus
}
#endif
