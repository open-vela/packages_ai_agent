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
 * BLE GATT Command Handler
 *
 * Processes JSON commands received from the companion Android App
 * via BLE GATT NUS RX characteristic.
 *
 * Supported commands:
 *   {"cmd":"wifi_config","ssid":"...","password":"..."}
 *   {"cmd":"ping"}
 *   {"cmd":"status"}
 */

#include "ble_cmd_handler.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>

#include "infra/ble_gatt.h"
#include "infra/network_manager.h"

#define TAG "ble_cmd"

/* Simple JSON string value extractor (no external dependency) */
static int json_get_string(const char* json, const char* key,
    char* out, size_t out_size)
{
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);

    const char* start = strstr(json, pattern);
    if (!start) {
        /* Try with space after colon */
        snprintf(pattern, sizeof(pattern), "\"%s\": \"", key);
        start = strstr(json, pattern);
        if (!start) {
            return -ENOENT;
        }
    }

    start = strchr(start, ':');
    if (!start) {
        return -EINVAL;
    }

    /* Skip ': "' or ':"' */
    start++;
    while (*start == ' ')
        start++;
    if (*start != '"') {
        return -EINVAL;
    }
    start++; /* Skip opening quote */

    const char* end = strchr(start, '"');
    if (!end) {
        return -EINVAL;
    }

    size_t len = end - start;
    if (len >= out_size) {
        return -ENOSPC;
    }

    memcpy(out, start, len);
    out[len] = '\0';
    return 0;
}

static void send_response(const char* json)
{
    ble_gatt_send((const uint8_t*)json, strlen(json));
}

static void handle_wifi_config(const char* json)
{
    char ssid[64];
    char password[128];

    if (json_get_string(json, "ssid", ssid, sizeof(ssid)) != 0) {
        syslog(LOG_ERR, "[%s] wifi_config: missing ssid\n", TAG);
        send_response("{\"status\":\"error\",\"msg\":\"missing ssid\"}");
        return;
    }

    if (json_get_string(json, "password", password, sizeof(password)) != 0) {
        /* Allow empty password for open networks */
        password[0] = '\0';
    }

    syslog(LOG_INFO, "[%s] WiFi config: ssid=%s\n", TAG, ssid);

#ifdef CONFIG_AI_AGENT_WIFI
    int ret = network_wifi_connect(NULL, ssid, password);
    if (ret == 0) {
        syslog(LOG_INFO, "[%s] WiFi connected successfully\n", TAG);
        send_response("{\"status\":\"ok\",\"msg\":\"wifi connected\"}");
    } else {
        syslog(LOG_ERR, "[%s] WiFi connect failed: %d\n", TAG, ret);
        char resp[128];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"error\",\"msg\":\"wifi failed: %d\"}", ret);
        send_response(resp);
    }
#else
    syslog(LOG_WARNING, "[%s] WiFi not supported on this build\n", TAG);
    send_response("{\"status\":\"error\",\"msg\":\"wifi not supported\"}");
#endif
}

static void handle_ping(void)
{
    send_response("{\"status\":\"ok\",\"msg\":\"pong\"}");
}

static void handle_status(void)
{
    char resp[256];
    bool net = network_is_connected();
    const char* ip = net ? network_get_ip() : "none";

    snprintf(resp, sizeof(resp),
        "{\"status\":\"ok\",\"network\":%s,\"ip\":\"%s\"}",
        net ? "true" : "false", ip ? ip : "none");
    send_response(resp);
}

/* ── Public API ────────────────────────────────────────────── */

void ble_cmd_handler_recv(const uint8_t* data, uint16_t len, void* user_data)
{
    (void)user_data;

    if (!data || len == 0) {
        return;
    }

    /* Null-terminate for string operations */
    char buf[512];
    size_t copy_len = (len < sizeof(buf) - 1) ? len : sizeof(buf) - 1;
    memcpy(buf, data, copy_len);
    buf[copy_len] = '\0';

    syslog(LOG_INFO, "[%s] Received: %s\n", TAG, buf);

    /* Extract command */
    char cmd[32];
    if (json_get_string(buf, "cmd", cmd, sizeof(cmd)) != 0) {
        syslog(LOG_WARNING, "[%s] No 'cmd' field in message\n", TAG);
        send_response("{\"status\":\"error\",\"msg\":\"missing cmd\"}");
        return;
    }

    if (strcmp(cmd, "wifi_config") == 0) {
        handle_wifi_config(buf);
    } else if (strcmp(cmd, "ping") == 0) {
        handle_ping();
    } else if (strcmp(cmd, "status") == 0) {
        handle_status();
    } else {
        syslog(LOG_WARNING, "[%s] Unknown command: %s\n", TAG, cmd);
        char resp[128];
        snprintf(resp, sizeof(resp),
            "{\"status\":\"error\",\"msg\":\"unknown cmd: %s\"}", cmd);
        send_response(resp);
    }
}
