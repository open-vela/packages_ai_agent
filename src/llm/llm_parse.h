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

/**
 * llm_parse.h — Tool call parsing and response helpers.
 */

#include "cJSON.h"
#include "llm/llm_proxy.h"

/* Convert internal tools JSON to OpenAI function-calling format.
 * Returns a cJSON array to attach to the request body, or NULL. */
cJSON* build_openai_tools_array(const char* tools_json);

/* XML fallback parsers for non-standard models */
void parse_xml_tool_calls(llm_response_t* resp);
void parse_ns_xml_tool_calls(llm_response_t* resp);
void parse_dsml_tool_calls(llm_response_t* resp);

/* Extract OpenAI tool_calls from response message */
void extract_openai_tool_calls(cJSON* message, llm_response_t* resp);
