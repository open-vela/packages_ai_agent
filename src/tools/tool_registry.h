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
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char* name;
    const char* description;
    const char* input_schema_json;
    int (*execute)(const char* input_json, char* output, size_t output_size);
} agent_tool_t;

/* ── Schema builder macros (compile-time JSON Schema generation) ─── */

#define TOOL_SCHEMA_BEGIN() "{\"type\":\"object\",\"properties\":{"

#define TOOL_SCHEMA_END() "},\"required\":[]}"

#define TOOL_SCHEMA_END_REQUIRED(required_list) \
    "},\"required\":[" required_list "]}"

#define TOOL_NO_PARAMS "{\"type\":\"object\",\"properties\":{},\"required\":[]}"

#define TOOL_PARAM_STR(name, desc) \
    "\"" name "\":{\"type\":\"string\",\"description\":\"" desc "\"}"

#define TOOL_PARAM_NUM(name, desc) \
    "\"" name "\":{\"type\":\"number\",\"description\":\"" desc "\"}"

#define TOOL_PARAM_BOOL(name, desc) \
    "\"" name "\":{\"type\":\"boolean\",\"description\":\"" desc "\"}"

#define TOOL_PARAM_ENUM(name, desc, enum_values)                      \
    "\"" name "\":{\"type\":\"string\",\"description\":\"" desc "\"," \
    "\"enum\":[" enum_values "]}"

/* ── Registration macros ───────────────────────────────────────────── */

#define REGISTER_TOOL(tname, desc, schema, fn) \
    do {                                       \
        agent_tool_t _tool_##fn = {         \
            .name = tname,                     \
            .description = desc,               \
            .input_schema_json = schema,       \
            .execute = fn,                     \
        };                                     \
        register_tool(&_tool_##fn);            \
    } while (0)

#define REGISTER_TOOL_NO_PARAMS(tname, desc, fn) \
    REGISTER_TOOL(tname, desc, TOOL_NO_PARAMS, fn)

/* ── External tool provider callback (breaks circular dependency) ──── */

/* The provider types move to the public header so a board can register tools
 * without reaching into src/.  Keep them declared in exactly one place: the
 * callback types are passed by value to tool_registry_register_provider, so a
 * second, structurally identical definition would be a different type. */
#include "tools/tool_provider.h"

int   tool_registry_init(void);
char *tool_registry_get_tools_json(void);  /* caller must free() */
void  tool_registry_rebuild_json(void);    /* force rebuild tools JSON now */
void  tool_registry_invalidate(void);      /* mark tools dirty for rebuild */
void  tool_registry_cleanup(void);         /* free tools JSON cache */
int   tool_registry_execute(const char *name, const char *input_json,
                                   char *output, size_t output_size);

#ifdef __cplusplus
}
#endif
