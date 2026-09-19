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

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#pragma once

#include "agent_compat.h"

int ws_server_start(void);
int ws_server_send(const char *chat_id, const char *text);
int ws_server_send_json(const char *chat_id, const char *json);
/* 广播控制帧给所有已连接客户端。用于事件源头在设备侧、却不知道宿主
 * chat_id 的场景（屏幕上的 PTT 按钮就是：拿不到 chat_id，建屏时宿主
 * 甚至可能还没连上来）。返回 OK 表示至少发出去一份。 */
int ws_server_broadcast_json(const char *json);
int ws_server_send_binary(const char *chat_id, const void *data, size_t len);
int ws_server_stop(void);
