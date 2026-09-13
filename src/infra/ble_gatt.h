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

#ifndef __BLE_GATT_H__
#define __BLE_GATT_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/**
 * BLE GATT Data Channel - Custom GATT Service for data exchange
 *
 * Architecture (Nordic UART Service pattern):
 *   Phone App writes to RX Characteristic -> ble_gatt_recv_cb -> AI Agent
 *   AI Agent calls ble_gatt_send() -> TX Characteristic notify -> Phone App
 *
 * Custom Service UUID: 6e400001-b5a3-f393-e0a9-e50e24dcca9e (NUS)
 *   RX Char UUID:      6e400002-b5a3-f393-e0a9-e50e24dcca9e (Write)
 *   TX Char UUID:      6e400003-b5a3-f393-e0a9-e50e24dcca9e (Notify)
 *
 * Works with BLE (no classic Bluetooth required), suitable for:
 *   - iOS devices (no SPP support)
 *   - Low-power BLE-only chips
 *   - Direct data exchange without TUN/network proxy
 */

/**
 * Callback when data is received from phone via GATT write
 *
 * @param data  Received data buffer
 * @param len   Data length
 * @param user_data  User context
 */
typedef void (*ble_gatt_recv_cb_t)(const uint8_t* data, uint16_t len,
    void* user_data);

/**
 * Callback when connection state changes
 *
 * @param connected  true if connected, false if disconnected
 * @param user_data  User context
 */
typedef void (*ble_gatt_conn_cb_t)(bool connected, void* user_data);

/** Configuration */
typedef struct {
    const char* device_name; /* BLE device name for advertising */
    ble_gatt_recv_cb_t recv_cb; /* Data receive callback (required) */
    ble_gatt_conn_cb_t conn_cb; /* Connection state callback (optional) */
    void* user_data; /* User context for callbacks */
} ble_gatt_config_t;

/** Initialize BLE GATT data channel */
int ble_gatt_init(const ble_gatt_config_t* config);

/** Deinitialize BLE GATT data channel */
int ble_gatt_deinit(void);

/** Check if a GATT client is connected */
bool ble_gatt_is_connected(void);

/**
 * Send data to connected phone via TX Characteristic notify
 *
 * @param data  Data to send
 * @param len   Data length (max MTU - 3)
 * @return bytes sent on success, <0 on error
 */
int ble_gatt_send(const uint8_t* data, uint16_t len);

/** Get current negotiated MTU */
uint16_t ble_gatt_get_mtu(void);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_GATT_H__ */
