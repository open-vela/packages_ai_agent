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

int tool_get_heartrate_execute(const char *input_json, char *output, size_t output_size);
int tool_get_steps_execute(const char *input_json, char *output, size_t output_size);

/* 血氧: 有上次结果直接报数; 没有则起一次异步测量(约 20s 后由
 * health/spo2_measure.c 上屏 + 语音播报), 工具本身不阻塞。
 * 实现体在 CONFIG_AI_AGENT_LVGL_UI 下编译(与 spo2_measure.c 同条件)。 */
int tool_get_spo2_execute(const char *input_json, char *output, size_t output_size);

/* 麦克风自检: 录 3 秒 PCM 并给出 mic OK / mic NO-RESPONSE 结论。
 * 实现在 src/health/mic_selfcheck.c (与 hr_measure.c 同目录)。 */
int tool_mic_selfcheck_execute(const char *input_json, char *output, size_t output_size);
