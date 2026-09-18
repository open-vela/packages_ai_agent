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
 * Registering tools from outside packages/ai_agent.
 *
 * Built-in tools are added inside tool_registry_init(), which lives in this
 * package and therefore cannot name anything the board adds on top.  A
 * provider breaks that direction: the board hands over two callbacks before
 * tool_registry_init() runs, and the registry folds the returned definitions
 * into the tools JSON it sends to the model.
 *
 * The provider owns parameter validation.  tool_guard.c limits call counts and
 * input size but does not check a tool's schema at runtime, so an executor has
 * to reject out-of-range or oversized arguments itself.
 */

#pragma once

#include "agent_compat.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Callback type for external tool providers.
 * Returns a JSON array string of tools (caller must free), or NULL if none.
 * Each element carries name, description and input_schema, the same three
 * keys the built-in registry emits.
 */
typedef char* (*tool_provider_fn)(void);

/**
 * Callback type for external tool executors.
 * Returns OK if tool was found and executed, ERROR otherwise.  Returning
 * ERROR for a name the provider does not own lets the next provider try.
 */
typedef int (*tool_executor_fn)(const char* name, const char* input_json,
                                char* output, size_t output_size);

/**
 * Register an external tool provider (e.g., node_manager, mcp_client).
 * The provider's get_tools_json callback will be called during tools JSON build.
 * The executor callback will be called as fallback during tool execution.
 * Max 4 providers supported.
 */
void tool_registry_register_provider(const char* name,
                                     tool_provider_fn get_tools,
                                     tool_executor_fn execute);

/**
 * The tools JSON the model is being offered, after every builtin and provider
 * has contributed.  A provider host needs this to check that its own
 * definitions were merged and are still parseable, which is otherwise only
 * observable by reading the request the board sends.  Caller must free().
 */
char* tool_registry_get_tools_json(void);

/**
 * Run one tool by name, going through the same guard and audit path the ReAct
 * loop uses.  Returns OK if the tool was found and executed.
 *
 * This exists for board diagnostics: it is how an on-device acceptance run can
 * show what a read-only tool answers without depending on a working LLM.
 */
int tool_registry_execute(const char* name, const char* input_json,
                          char* output, size_t output_size);

#ifdef __cplusplus
}
#endif
