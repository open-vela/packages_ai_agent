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

#ifndef __BLE_CMD_HANDLER_H__
#define __BLE_CMD_HANDLER_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * BLE GATT receive callback - handles JSON commands from companion App.
 *
 * Register this as the recv_cb in ble_gatt_config_t:
 *   ble_gatt_config_t cfg = { .recv_cb = ble_cmd_handler_recv };
 *
 * Supported commands:
 *   {"cmd":"wifi_config","ssid":"...","password":"..."}
 *   {"cmd":"ping"}
 *   {"cmd":"status"}
 */
void ble_cmd_handler_recv(const uint8_t* data, uint16_t len, void* user_data);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_CMD_HANDLER_H__ */
