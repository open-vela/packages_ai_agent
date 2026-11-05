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
 * A question like "what is the UPS load" normally costs two model round
 * trips: the first picks vg_point_read, the second restates the number the
 * tool already returned.  The second exists only to reword a decided value
 * and carries the same multi-second server wait as the first.
 *
 * The tool output is machine-formatted, so the fields can be rendered here
 * without guessing:
 *
 *   vgquery: point id=ups_load name=UPS负载 value=42 unit=% ok=1 age_ms=900
 *
 * Only that exact shape with a fresh, successful sample is handled.  An
 * ambiguous query, a missing table, no sample, a failed read, a stale sample
 * or a question that asks for wording (why / compare / normal? / advice) all
 * return NULL and take the normal LLM path: those need explanation rather
 * than echoing a number.
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Why a round did or did not take the shortcut.
 *
 * The caller logs the verdict for every vg_point_read round, so a missed hit
 * can be told apart from a tool failure without re-running the test.
 */
typedef enum {
    VG_POINT_REPLY_HIT = 0,
    /* The tool returned a table, candidate list or error line instead of one
     * point.  AMBIGUOUS and NOT_FOUND are separated from the rest because
     * they are the normal answer to a short name that matches more than one
     * point, not a failure. */
    VG_POINT_REPLY_AMBIGUOUS,
    VG_POINT_REPLY_NOT_FOUND,
    VG_POINT_REPLY_NOT_TOOL_OUTPUT,
    VG_POINT_REPLY_INCOMPLETE,
    VG_POINT_REPLY_READ_FAILED,
    VG_POINT_REPLY_NO_SAMPLE,
    VG_POINT_REPLY_STALE,
    VG_POINT_REPLY_NOT_DIRECT,
    VG_POINT_REPLY_RENDER_ERROR
} vg_point_reply_verdict_t;

/**
 * Build a user-facing reply from one vg_point_read result.
 *
 * The rendering keeps the point name, the value verbatim, the unit and the
 * sample age, and states that the point is online.  It never rounds,
 * rescales or recomputes the value: the path saves one round trip, it must
 * not become a second source of truth.
 *
 * @return a heap string the caller owns, or NULL when the reply must go
 *         through the model.
 */
char* vg_point_reply_from_tool_output(const char* tool_output);

/**
 * Same as vg_point_reply_from_tool_output(), also taking the user's question
 * and reporting the verdict.
 *
 * @param tool_output  Raw vg_point_read output.
 * @param question     The user message that produced this round.
 * @param verdict      Optional; receives why the shortcut fired or not.
 * @return a heap string the caller owns, or NULL to keep the model round.
 */
char* vg_point_reply_render(const char* tool_output, const char* question,
    vg_point_reply_verdict_t* verdict);

/** Stable name for a verdict, for the trace log. */
const char* vg_point_reply_verdict_name(vg_point_reply_verdict_t verdict);

/**
 * True when a question is only asking for a current reading.
 *
 * Explanation requests (why / cause / normal? / compare / trend / advice /
 * threshold), report requests and multi-point questions must reach the
 * model even when the tool happens to resolve one point, because a value
 * echo would answer a different question than the one asked.
 *
 * @return true when the single-point shortcut may serve this question.
 */
bool vg_point_reply_is_direct_question(const char* question);

#ifdef __cplusplus
}
#endif
