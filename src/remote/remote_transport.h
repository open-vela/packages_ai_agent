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

/* Pluggable link transport. A transport moves complete JSON messages and knows
 * nothing about their contents. MQTT is the first one; a newline-framed USB CDC
 * transport would implement the same single operation. */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct remote_transport_ops_s {
    bool (*send_json)(void* context, const char* json, size_t length);
};

struct remote_transport_s {
    const struct remote_transport_ops_s* ops;
    void* context;
};

static inline bool remote_transport_send(struct remote_transport_s* transport,
    const char* json, size_t length)
{
    return transport != NULL && transport->ops != NULL && transport->ops->send_json != NULL && transport->ops->send_json(transport->context, json, length);
}

#ifdef __cplusplus
}
#endif
