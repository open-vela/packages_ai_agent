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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback type for receiving replies from the AI agent.
 * @param chat_id     Unique identifier for the conversation turn.
 * @param content     Reply content string.
 * @param extra_info  JSON string with extended info (e.g. {"rpk":"..."}).
 *                    NULL if no extra info is available.
 * @param tool_calls  JSON array string of tool calls made by the agent
 *                    (e.g. [{"name":"generate_widget","result":"..."}]).
 *                    NULL if no tools were called.
 * @param userdata    User-provided context pointer.
 */
typedef void (*velaclaw_quickapp_reply_cb)(const char *chat_id,
                                           const char *content,
                                           const char *extra_info,
                                           const char *tool_calls,
                                           void *userdata);

/**
 * Register a callback to receive agent replies on the quickapp bridge.
 * @param cb       Callback function invoked when a reply arrives.
 * @param userdata Opaque pointer forwarded to the callback.
 */
void velaclaw_quickapp_bridge_register(velaclaw_quickapp_reply_cb cb,
                                       void *userdata);

/**
 * Unregister the previously registered reply callback.
 */
void velaclaw_quickapp_bridge_unregister(void);

/**
 * Send a query to the AI agent via the quickapp bridge.
 * @param chat_id  Unique identifier for this request.
 * @param query    The user query string.
 * @return 0 on success, negative errno on failure.
 */
int velaclaw_quickapp_bridge_ask(const char *chat_id, const char *query);

int velaclaw_quickapp_bridge_get_status(char *output, size_t output_size);
int velaclaw_quickapp_bridge_call_tool(const char *app_id,
                                       const char *tool,
                                       const char *args_json,
                                       char *output,
                                       size_t output_size);

#ifdef __cplusplus
}
#endif
