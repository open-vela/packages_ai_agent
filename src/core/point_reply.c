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

#include "core/point_reply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char* vg_point_reply_from_tool_output(const char* tool_output)
{
    char id_buf[64], name_buf[96], value_buf[32], unit_buf[16];
    const char* unit;
    const char* age;
    char out[256];
    const char* id;
    const char* name;
    const char* value;
    const char* ok;
    unsigned long age_ms = 0;
    bool stale;
    int n;

    if (tool_output == NULL
        || strncmp(tool_output, "vgquery: point ", 15) != 0) {
        return NULL;
    }

    /* Every field must be present.  A reply built from a partial line would
     * state a value without saying how old it is, which is worse than the
     * extra round-trip. */

    id = strstr(tool_output, " id=");
    name = strstr(tool_output, " name=");
    value = strstr(tool_output, " value=");
    unit = strstr(tool_output, " unit=");
    ok = strstr(tool_output, " ok=");
    age = strstr(tool_output, " age_ms=");

    if (!id || !name || !value || !unit || !ok || !age) {
        return NULL;
    }

    /* Only a successful read with a sample is echoed.  read_failed and
     * no_sample carry reasons the user needs explained, not repeated. */

    if (strncmp(ok, " ok=1", 5) != 0) {
        return NULL;
    }

    if (sscanf(id, " id=%63s", id_buf) != 1
        || sscanf(name, " name=%95s", name_buf) != 1
        || sscanf(value, " value=%31s", value_buf) != 1
        || sscanf(unit, " unit=%15s", unit_buf) != 1
        || sscanf(age, " age_ms=%lu", &age_ms) != 1) {
        return NULL;
    }

    stale = strstr(tool_output, " stale=1") != NULL;

    /* The value is echoed exactly as the tool produced it: no rounding, no
     * unit conversion, no recomputation.  This path removes a model
     * round-trip; it must not become a second source of truth. */

    n = snprintf(out, sizeof(out), "%s当前读数 %s %s",
        strcmp(name_buf, "-") != 0 ? name_buf : id_buf,
        strcmp(value_buf, "-") != 0 ? value_buf : "无有效读数",
        strcmp(unit_buf, "-") != 0 ? unit_buf : "");

    if (n < 0 || (size_t)n >= sizeof(out)) {
        return NULL;
    }

    if (stale) {
        snprintf(out + n, sizeof(out) - (size_t)n,
            "（采样已过期，%.0f 秒前）", (double)age_ms / 1000.0);
    } else {
        snprintf(out + n, sizeof(out) - (size_t)n,
            "（%.0f 秒前采样）", (double)age_ms / 1000.0);
    }

    return strdup(out);
}
