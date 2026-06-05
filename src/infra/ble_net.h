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

#ifndef __BLE_NET_H__
#define __BLE_NET_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/**
 * BLE Network Channel - SPP + TUN based network proxy
 *
 * Architecture:
 *   App (TCP/IP) -> TUN device -> SPP -> Phone App -> Internet
 *
 * Requires companion app on phone to proxy network traffic.
 */

/** Initialize BLE network channel (TUN + SPP server) */
int ble_net_init(void);

/** Deinitialize BLE network channel */
int ble_net_deinit(void);

/** Check if BLE network is connected */
bool ble_net_is_connected(void);

/** Send data to phone (called by TUN read) */
int ble_net_send(const uint8_t* data, uint16_t len);

/** Receive data from phone (called by SPP receive, writes to TUN) */
int ble_net_receive(const uint8_t* data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_NET_H__ */
