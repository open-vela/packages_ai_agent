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

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Tool security classification */
typedef enum {
    TOOL_SEC_SAFE,       /* read-only, no side effects */
    TOOL_SEC_MODERATE,   /* writes data dir, network calls */
    TOOL_SEC_SENSITIVE   /* shell, file write, device control */
} tool_security_level_t;

/* Guard check result */
typedef enum {
    GUARD_ALLOW,
    GUARD_DENY_DISABLED,     /* tool disabled by config */
    GUARD_DENY_RATE_LIMIT,   /* too many calls in window */
    GUARD_DENY_INPUT_SIZE,   /* input exceeds max length */
    GUARD_DENY_INPUT_INVALID /* malformed input detected */
} tool_guard_result_t;

int  tool_guard_init(void);
void tool_guard_cleanup(void);

/* Check if a tool call should be allowed.  For a sensitive tool, an allowed
 * result atomically reserves one rate-limit slot before execution, so calls
 * that later fail still count and concurrent callers cannot pass the limit.
 */
tool_guard_result_t tool_guard_check(const char *tool_name,
                                     const char *input_json,
                                     size_t input_len);

/* Runtime enable/disable a tool via config store. */
int  tool_guard_set_enabled(const char *tool_name, bool enabled);
bool tool_guard_is_enabled(const char *tool_name);

/* Sanitize log output — redact sensitive fields. */
void tool_guard_sanitize_log(const char *input_json, char *safe_buf,
                             size_t safe_size);

/* Check user message for prompt injection patterns.
 * Returns true if injection detected. */
bool tool_guard_check_injection(const char *user_message);

#ifdef __cplusplus
}
#endif
