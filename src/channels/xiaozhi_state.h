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

/**
 * xiaozhi_state.h — XiaoZhi channel state machine definitions.
 *
 * State flow:
 *   Disconnected → MqttConnecting → MqttConnected →
 *   ChannelOpened → UdpConnected → AudioStreaming
 */

typedef enum {
    XZ_STATE_DISCONNECTED = 0,
    XZ_STATE_CONNECTING,       /* was MQTT_CONNECTING, now WS connecting */
    XZ_STATE_CONNECTED,        /* was MQTT_CONNECTED, now WS connected */
    XZ_STATE_CHANNEL_OPENED,
    XZ_STATE_UDP_CONNECTED,    /* reserved for MQTT+UDP mode */
    XZ_STATE_AUDIO_STREAMING,  /* reserved for MQTT+UDP mode */
    XZ_STATE_MAX
} xiaozhi_state_t;

static inline const char* xiaozhi_state_name(xiaozhi_state_t s)
{
    static const char* names[] = {
        "Disconnected",
        "Connecting",
        "Connected",
        "ChannelOpened",
        "UdpConnected",
        "AudioStreaming",
    };

    if (s >= 0 && s < XZ_STATE_MAX) {
        return names[s];
    }

    return "Unknown";
}
