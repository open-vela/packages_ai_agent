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

/* Wire codec: the only layer that knows the link is carried as JSON.
 *
 * Inbound messages are validated here and handed to the session as decoded
 * structures; the session and the state machine never see JSON. Keeping this
 * layer separate is what makes a future direct-ACP backend an added codec
 * rather than a rewrite of the state machine.
 */

#pragma once

#include "remote/remote_link.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The encoded message cap lives with the protocol constants in remote_link.h
 * (REMOTE_MESSAGE_MAX) so the state machine, the session's staging buffer and
 * this codec cannot drift apart. */

/* Buffer sizes callers should use for the builders below. */
#define REMOTE_CODEC_HELLO_BUF 128
#define REMOTE_CODEC_DECISION_BUF 512
#define REMOTE_CODEC_USAGE_QUERY_BUF 192
#define REMOTE_CODEC_MODEL_SELECT_BUF 320

struct remote_session_s;

/* ── Inbound ───────────────────────────────────────────────────────── */

/* Validates one complete wire message and applies it to the session. Silently
 * ignores anything malformed, oversized, stale, or out of session. `now_ms` is
 * only used to refresh authority-snapshot liveness. */
void remote_codec_receive(struct remote_session_s* session,
    const char* payload, size_t length, uint32_t now_ms);

/* ── Outbound ──────────────────────────────────────────────────────── */

bool remote_codec_build_hello(const struct remote_link_s* link, char* out,
    size_t out_size);
bool remote_codec_build_decision(const struct remote_link_s* link,
    enum remote_decision_e decision, char* out, size_t out_size);
bool remote_codec_build_usage_query(const struct remote_link_s* link,
    char* out, size_t out_size);
bool remote_codec_build_prompt_submit(const struct remote_link_s* link,
    const char* request_id, const char* text, char* out, size_t out_size);
bool remote_codec_build_model_select(const struct remote_link_s* link,
    const char* model, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif
