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

#ifndef AI_AGENT_TOOL_REST_H
#define AI_AGENT_TOOL_REST_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 休息状态（体动估算）。
 *
 *   {"state":"awake|quiet|night","quiet_s":N,
 *    "last_night":{"start":..,"end":..,"dur_s":..,"estimate":"motion_only"},
 *    "note":"基于体动估算，非医疗级"}
 *
 * 没有昨夜数据时 last_night 为 null。数据来自 health/rest_track.c；
 * 这里只做 JSON 组装，不重新估算、不补默认值。
 *
 * 返回 OK(0) 成功，output 是合法 JSON；ERROR(-1) 失败，output 是
 * {"error":"..."} —— 与 tool_health.c 的工具约定一致。
 */
int tool_get_rest_execute(const char *input_json, char *output,
                          size_t output_size);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_TOOL_REST_H */
