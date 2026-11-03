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

#include <stdbool.h>
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
 *   {"cmd":"health"}                                   旧 health 回包（bat 为演示固定值 100）
 *   {"cmd":"wifi_config","ssid":"...","password":"..."}
 *   {"cmd":"ping"} / {"cmd":"status"}
 *   {"cmd":"chat","text":"..."}                         evt=chat，回答几十秒后异步回
 *   {"cmd":"chat_history"}                              evt=chat_history
 *   {"cmd":"alarm_list"} / {"cmd":"alarm_add","name":"...","at":<epoch>}
 *   {"cmd":"alarm_del","id":"..."}                      以上三条 evt=alarm_*
 *   {"cmd":"expression_get"} / {"cmd":"expression_set","index":N}
 *   {"cmd":"settings_get"} / {"cmd":"settings_set","brightness":N|"dnd":1}
 *
 * 见 docs_ble/98_ble_extended_protocol.md 的 FW-A 契约。
 */
void ble_cmd_handler_recv(const uint8_t* data, uint16_t len, void* user_data);

/**
 * 把 agent 的一条回答回给手机 App（evt=chat）。
 *
 * 由 outbound 派发线程在 channel=="ble" 时调用：字符串做 JSON 转义
 * （引号/反斜杠/换行/控制字符），中文原样 UTF-8；一次 ble_gatt_send
 * 交出去，NUS 的 (MTU-3) 分片由协议栈处理。超过单帧上限（1600B）时按
 * UTF-8 字符边界截断，保证发出去的始终是一条合法 JSON。
 *
 * @param text        回答文本（NULL 视为空串）
 * @param from_local  true = 端侧模型/本地工具答的，false = 云端
 */
void ble_send_chat_reply(const char* text, bool from_local);

/**
 * 测试用回包出口（串口自测）：
 *   typedef void (*ble_test_sink_t)(const char* json);
 * 注册后所有回包（含 ble_send_chat_reply）不再走 BLE notify，直接交给 sink，
 * 便于 NSH 的 "ui ble ..." 把 JSON 打到串口。传 NULL 恢复走 BLE。
 * 只在启动阶段注册一次，不要求线程安全。
 */
typedef void (*ble_test_sink_t)(const char* json);
void ble_cmd_handler_set_test_sink(ble_test_sink_t sink);

#ifdef __cplusplus
}
#endif

#endif /* __BLE_CMD_HANDLER_H__ */
