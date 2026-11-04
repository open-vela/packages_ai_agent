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

#pragma once

#include <stddef.h>

/*
 * 每日健康摘要工具（"昨天走了多少步 / 最近心率 / 昨晚休息 / 今天天气"的组合问题）。
 *
 * 与 ui/health_summary.c 同源：上屏卡片用的那套 collect + 文案生成器，这里
 * 原样再跑一遍（不要求今天已经展示过），所以工具和气泡说的话不会两套口径。
 * 数据全部读现成缓存，不发网络请求、不触发测量、不写 health_store。
 *
 *   输入: 无参数（input_json 忽略）
 *   输出: {"date":"YYYY-MM-DD",
 *          "steps_yesterday":N,
 *          "steps_source":"yesterday|latest|none",
 *          "steps_date":"YYYY-MM-DD",              // source != none 时才有
 *          "hr":{"min":..,"max":..,"count":..}|null,
 *          "rest_s":N|null,
 *          "weather":{"temp_c":..,"code":..,"desc":".."}|null,
 *          "text":"..."}
 *
 *   - steps_yesterday: 没有昨天那条时用最近一次旧记录（steps_source=latest，
 *     日期在 steps_date 里），完全没历史时为 0 且 steps_source=none；
 *   - hr: 最近 24 h 的 min/max/count；没有有效测量时为 null（不报 0 bpm）；
 *   - rest_s: rest.json 的 last_night.dur_s；没有/非法时 null；
 *   - weather: 共享天气缓存的快照；没有过成功数据时 null。
 *
 * 失败（时钟不可信 / 内存不足 / 输出缓冲太小）：{"error":"..."} 且返回 ERROR。
 * 时钟不可信时不编日期 —— 年份不在 2024..2027 一律 {"error":"clock not set"}。
 *
 * 实现体在 src/tools/tool_health_summary.c，由 tool_registry 注册。
 */
int tool_get_health_summary_execute(const char *input_json, char *output,
                                    size_t output_size);
