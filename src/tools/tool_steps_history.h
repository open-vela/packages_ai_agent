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

#ifndef AI_AGENT_TOOL_STEPS_HISTORY_H
#define AI_AGENT_TOOL_STEPS_HISTORY_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 步数历史: 直接透传 health/pedometer.c 的 JSON。
 *
 *   {"date":"2026-10-31","steps":1234,
 *    "history":[{"date":"2026-10-30","steps":4321}]}
 *
 * 和 get_steps 的区别: get_steps 读 uORB 的实时值(只有今天、且在话题
 * 没发布时返回 sensor not available), 这个读落盘的那一份 —— 含最近 7 天。
 *
 * 返回 OK(0) 成功, output 是合法 JSON; ERROR(-1) 失败, output 是
 * {"error":"..."} 形式 —— 与 tool_health.c 的工具约定一致。
 */
int tool_get_steps_history_execute(const char *input_json, char *output,
                                   size_t output_size);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_TOOL_STEPS_HISTORY_H */
