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

#ifndef __BLE_GATT_NET_H__
#define __BLE_GATT_NET_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>

/**
 * BLE GATT Network Channel - NUS + TUN based network proxy
 *
 * Architecture:
 *   App (TCP/IP) -> TUN device -> GATT NUS -> Phone App -> Internet
 *
 * The SF32LB52 LCPU firmware does not implement BR/EDR (classic BT),
 * so the data channel uses BLE GATT NUS (Nordic UART Service) instead
 * of SPP. Requires the companion app on the phone to proxy traffic.
 */

/** Initialize BLE GATT network channel (TUN + NUS server + advertising) */
int ble_gatt_net_init(void);

/** Deinitialize BLE GATT network channel */
int ble_gatt_net_deinit(void);

/** Check if BLE GATT network is connected (phone attached) */
bool ble_gatt_net_is_connected(void);

/** Send data to phone (called by TUN read) */
int ble_gatt_net_send(const uint8_t* data, uint16_t len);

/** Receive data from phone (writes to TUN) */
int ble_gatt_net_receive(const uint8_t* data, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_GATT_NET_H__ */
