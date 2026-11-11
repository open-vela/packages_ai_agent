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

/**
 * ui_rpmsg_server.h — CP-side AP→CP UI RPMsg receiver (路 B).
 * Called from cp_ui_main after LVGL is up so the CP starts listening for
 * UI commands (bubbles/toasts) from the AP agent over RPMsg.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/** Open the UI RPMsg endpoint.  Idempotent.  Returns 0 on success. */
int ui_rpmsg_server_init(void);

#ifdef __cplusplus
}
#endif
