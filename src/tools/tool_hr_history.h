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
 * 心率历史查询工具（"我的心率历史" 的落点）。
 *
 * 数据源是 hr_measure.c 里持久化的 288 条环形缓冲
 * (/data/ai_agent/health/hr_history.json)，本工具只做裁剪和重打包：
 *
 *   输入: 无参数（input_json 忽略）
 *   输出: {"latest_bpm":N,"latest_ts":T,"count":N,
 *          "items":[{"t":...,"bpm":...}, ...]}
 *
 *   - latest_*: 最新一条的时间和 bpm；
 *   - count:    历史总条数（不是 items 的长度，items 最多 20 条）；
 *   - items:    最近 20 条，旧 -> 新。
 *
 * 失败（读不到历史 / 序列化失败 / 输出缓冲太小）：
 *   {"error":"..."} 且返回 ERROR。
 * 历史为空也按失败处理 —— 本仓的诚实性红线是"没有数据就不报数"，
 * 不返回 0 bpm 让上层误以为测到了。
 *
 * 实现体在 src/tools/tool_hr_history.c，由 tool_registry 注册。
 */
int tool_get_heartrate_history_execute(const char *input_json, char *output,
                                       size_t output_size);
