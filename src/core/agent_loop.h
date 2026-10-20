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
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

int agent_loop_init(void);
int agent_loop_start(void);
/** Start agent_loop once; safe to call after late Wi-Fi connect. */
int agent_loop_ensure_started(void);
bool agent_loop_is_running(void);
/**
 * A/B switch for the single-point reply shortcut.
 *
 * A lone vg_point_read that resolved to one point, from a question that only
 * asks for the current value, is answered from the tool output instead of a
 * second model call.  On by default; "point_shortcut off" restores the
 * two-request path for re-measurement.  Switching also clears the response
 * cache, because a cached reply would be replayed under both arms.  Not
 * persisted.
 */
void agent_point_reply_set_enabled(bool enabled);
bool agent_point_reply_enabled(void);

#ifdef CONFIG_VG_HMI
void agent_loop_request_start(void);
bool agent_loop_is_requested(void);
#endif

#ifdef __cplusplus
}
#endif
