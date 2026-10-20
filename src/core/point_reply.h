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

/**
 * point_reply.h — Render a single-point tool result without a second LLM call.
 *
 * A question like "what is the UPS load" currently costs two model round
 * trips: the first picks vg_point_read, the second restates the number the
 * tool already returned.  The second exists only to reword a decided value
 * and carries the same multi-second server wait as the first.
 *
 * The tool output is machine-formatted, so the fields can be rendered here
 * without guessing:
 *
 *   vgquery: point id=ups_load name=UPS负载 value=42 unit=% ok=1 age_ms=900
 *
 * Only that exact shape with a successful sample is handled.  Ambiguity, a
 * missing table, no sample, a failed read or stale data all return NULL and
 * take the normal LLM path, because those need wording rather than echoing.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Build a user-facing reply from one vg_point_read result.
 *
 * @return a heap string the caller owns, or NULL when the reply must go
 *         through the model.
 */
char* vg_point_reply_from_tool_output(const char* tool_output);

#ifdef __cplusplus
}
#endif
