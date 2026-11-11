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

#include <stdio.h>
#include <string.h>

/* Append /chat/completions when a set_llm URL path stops at an API version
 * segment ("/v1", "/compatible-mode/v1", "/api/v3", "/api/paas/v4").
 * Complete paths ("/v1/chat/completions", "/v1/messages", "/v1beta") are
 * returned unchanged. */
static inline const char* llm_complete_path(const char* path, char* buf,
    size_t size)
{
    size_t len = strlen(path);
    while (len > 1 && path[len - 1] == '/')
        len--;

    size_t seg = len;
    while (seg > 0 && path[seg - 1] != '/')
        seg--;

    if (len - seg < 2 || path[seg] != 'v')
        return path;
    for (size_t i = seg + 1; i < len; i++) {
        if (path[i] < '0' || path[i] > '9')
            return path;
    }

    snprintf(buf, size, "%.*s/chat/completions", (int)len, path);
    return buf;
}
