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

/* Tools that let the local agent drive a remote ACP agent.
 *
 * There is deliberately no tool for answering the remote agent's permission
 * requests. Approving a tool call is a human act; a model able to both request
 * and approve would reduce the permission model to decoration. Those decisions
 * live in the NSH commands (and later a GPIO key) only.
 */

#pragma once

#include "agent_compat.h"

#include <stddef.h>

#ifdef CONFIG_AI_AGENT_REMOTE_CTRL

/**
 * Send one prompt to the remote agent and wait for the turn to finish.
 * Input: {"text": "..."}
 * Returns the agent's text, or a description of why nothing could be sent.
 */
int tool_remote_agent_prompt_execute(const char* input_json, char* output,
    size_t output_size);

/**
 * Report the remote link's state, summary, session billing, and whether a
 * human permission decision is pending. Read-only.
 */
int tool_remote_agent_status_execute(const char* input_json, char* output,
    size_t output_size);

#endif /* CONFIG_AI_AGENT_REMOTE_CTRL */
