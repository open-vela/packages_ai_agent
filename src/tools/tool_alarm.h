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
 * tool_alarm.h - Cloud-LLM tools that drive the on-device care scheduler.
 *
 * The on-device model has its own set_timer tool, but the cloud LLM's tool
 * set had no way to reach the watch's proactive alarm (preview 1 minute
 * before, wake-up call on time, escalation if nobody reacts). These two
 * tools close that gap so a Skill can hand a timing request to the device.
 */

#ifndef __APPS_AI_AGENT_TOOLS_TOOL_ALARM_H
#define __APPS_AI_AGENT_TOOLS_TOOL_ALARM_H

#include <stddef.h>

/**
 * Arm the watch alarm.
 * Input JSON: { minutes }
 *   minutes - whole minutes from now, 1..1440
 */
int tool_set_alarm_execute(const char *input_json, char *output,
                           size_t output_size);

/**
 * Cancel a pending alarm.
 * Input JSON: {} (no required fields)
 */
int tool_cancel_alarm_execute(const char *input_json, char *output,
                              size_t output_size);

#endif /* __APPS_AI_AGENT_TOOLS_TOOL_ALARM_H */
