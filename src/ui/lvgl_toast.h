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
 * lvgl_toast.h — Top-of-screen toast popup for notify cards.
 *
 * A toast is a temporary card overlaid on the active screen's top edge:
 * left color bar + title + body.  It auto-dismisses after a timeout
 * (fade-out animation) and can be dismissed early by tap.  It does NOT
 * enter the chat history — it is purely a glanceable transient notice,
 * used by the notify service for "Gerrit CI failed" / "new Jira issue"
 * style alerts.
 *
 * Thread-safety: lvgl_toast_show_async() may be called from any thread;
 * it hops into the LVGL thread via lv_async_call.  All widget creation
 * and deletion happens on the LVGL thread.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Show a toast popup.  Safe to call from any thread.
 *
 * @param title       Short title, e.g. "Gerrit" / "Jira" (copied internally)
 * @param body        Body text, e.g. "#9688768: audio CI failed" (copied)
 * @param bar_color   Left bar color, RGB 0xRRGGBB (e.g. 0x2196f3 blue)
 * @param duration_ms Auto-dismiss delay in ms (e.g. 4000)
 */
void lvgl_toast_show_async(const char *title, const char *body,
                           uint32_t bar_color, uint32_t duration_ms);

#ifdef __cplusplus
}
#endif
