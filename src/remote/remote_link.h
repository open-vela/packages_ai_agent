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

/* Remote-control link: portable protocol state core.
 *
 * This layer owns the authority model of a remote agent session and has no
 * transport, JSON, or OS dependency. It accepts already-decoded structures
 * from the codec layer and decides what the device is allowed to believe.
 *
 * The remote peer is a PC-side bridge that speaks the frozen DeskMate Link
 * contract on behalf of an ACP agent (mimocode / kiro-cli). The bridge, not
 * this device, is the final authority on permission decisions: everything
 * accepted here is still re-validated remotely.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REMOTE_NONCE_MAX 48
#define REMOTE_EPOCH_MAX 48
#define REMOTE_PROMPT_ID_MAX 40
#define REMOTE_TOOL_MAX 24
#define REMOTE_HINT_MAX 96
#define REMOTE_SUMMARY_MAX 128
#define REMOTE_BILLING_UNIT_MAX 16
#define REMOTE_REQUEST_ID_MAX 40
#define REMOTE_TURN_ID_MAX 40
#define REMOTE_PROMPT_TEXT_MAX 2048
#define REMOTE_OUTPUT_TEXT_MAX 1600
#define REMOTE_TRANSCRIPT_MAX 8192

/* A model identifier is "<provider>/<model>", and the model part may itself
 * contain slashes ("mify/zhipuai/glm-5.3-flash"). */
#define REMOTE_MODEL_MAX 64

/* How many models the device remembers so they can be picked by number. The
 * list is a convenience for typing at a console, not authority: the bridge
 * decides what is selectable and refuses anything else. Costs
 * REMOTE_MODEL_OPTIONS_MAX * (REMOTE_MODEL_MAX + 1) bytes of link state. */
#define REMOTE_MODEL_OPTIONS_MAX 32

/* Authority-snapshot liveness. A session that stops producing snapshots is
 * treated as down even while the TCP socket looks healthy. */
#define REMOTE_SNAPSHOT_TIMEOUT_MS 30000U

/* Frozen link limit, in both directions. A peer cannot force an unbounded
 * allocation on the device, and an outbound message that would exceed it is
 * refused rather than truncated. Note this is a limit on the encoded form:
 * escaping can expand a prompt well beyond its own length. */
#define REMOTE_MESSAGE_MAX 4096

enum remote_state_e {
    REMOTE_STATE_OFFLINE,
    REMOTE_STATE_IDLE,
    REMOTE_STATE_BUSY,
    REMOTE_STATE_ATTENTION,
    REMOTE_STATE_RESULT,
};

enum remote_decision_e {
    REMOTE_DECISION_ONCE,
    REMOTE_DECISION_DENY,
};

enum remote_charge_kind_e {
    REMOTE_CHARGE_CREDIT,
    REMOTE_CHARGE_CURRENCY,
};

enum remote_context_unit_e {
    REMOTE_CONTEXT_TOKEN,
    REMOTE_CONTEXT_PERCENT,
};

struct remote_prompt_s {
    char id[REMOTE_PROMPT_ID_MAX + 1];
    char tool[REMOTE_TOOL_MAX + 1];
    char hint[REMOTE_HINT_MAX + 1];
    bool can_once;
    bool can_deny;
};

struct remote_snapshot_s {
    const char* connection_nonce;
    const char* epoch;
    uint32_t seq;
    uint8_t running;
    const char* summary;
    const struct remote_prompt_s* prompt;
};

/* Usage is optional telemetry. It is independent from authority snapshots and
 * carries native units only: charge is the left billing slot and tokens the
 * right slot. It can never alter a pending permission. */
struct remote_usage_snapshot_s {
    const char* connection_nonce;
    const char* epoch;
    uint64_t seq;
    bool has_charge;
    enum remote_charge_kind_e charge_kind;
    double charge_amount;
    const char* charge_unit;
    bool has_tokens;
    uint64_t input_tokens;
    uint64_t output_tokens;
    uint64_t total_tokens;
    bool has_context;
    enum remote_context_unit_e context_unit;
    double context_used;
    double context_limit;
};

struct remote_usage_s {
    uint64_t last_seq;
    bool has_charge;
    enum remote_charge_kind_e charge_kind;
    double charge_amount;
    char charge_unit[REMOTE_BILLING_UNIT_MAX + 1];
    bool has_tokens;
    uint64_t input_tokens;
    uint64_t output_tokens;
    uint64_t total_tokens;
    bool has_context;
    enum remote_context_unit_e context_unit;
    double context_used;
    double context_limit;
};

struct remote_hello_ack_s {
    const char* connection_nonce;
    const char* epoch;
    bool chat_turns;
};

/* The selectable models, as reported by the bridge. `truncated` means the
 * bridge had more to offer than the link carries, so a name absent from the
 * list is not proof that it is unavailable. */
struct remote_models_s {
    const char* connection_nonce;
    const char* epoch;
    const char* current;
    const char* const* options;
    size_t count;
    bool truncated;
};

struct remote_prompt_ack_s {
    const char* connection_nonce;
    const char* epoch;
    const char* request_id;
    const char* turn_id;
    bool accepted;
};

struct remote_agent_output_s {
    const char* connection_nonce;
    const char* epoch;
    const char* request_id;
    const char* turn_id;
    uint32_t seq;
    const char* text;
};

struct remote_turn_result_s {
    const char* connection_nonce;
    const char* epoch;
    const char* request_id;
    const char* turn_id;
    const char* status;
};

struct remote_link_s {
    enum remote_state_e state;
    char nonce[REMOTE_NONCE_MAX + 1];
    char epoch[REMOTE_EPOCH_MAX + 1];
    uint32_t next_seq;
    char summary[REMOTE_SUMMARY_MAX + 1];
    struct remote_prompt_s prompt;
    bool has_prompt;
    uint32_t last_snapshot_ms;
    struct remote_usage_s usage;
    char chat_epoch[REMOTE_EPOCH_MAX + 1];
    bool chat_supported;
    bool turn_active;
    char turn_request_id[REMOTE_REQUEST_ID_MAX + 1];
    char turn_id[REMOTE_TURN_ID_MAX + 1];
    uint32_t last_output_seq;
    bool output_gap;
    char transcript[REMOTE_TRANSCRIPT_MAX + 1];
    /* Which model the remote agent is using, and what it may be switched to.
     * Display and convenience only: the bridge owns the decision and rejects a
     * value it does not offer, so stale contents here cannot select anything. */
    char model[REMOTE_MODEL_MAX + 1];
    char model_options[REMOTE_MODEL_OPTIONS_MAX][REMOTE_MODEL_MAX + 1];
    uint8_t model_count;
    bool models_truncated;
};

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void remote_link_init(struct remote_link_s* link, const char* nonce);
void remote_link_down(struct remote_link_s* link);

/* ── Inbound authority ─────────────────────────────────────────────── */

bool remote_link_snapshot(struct remote_link_s* link,
    const struct remote_snapshot_s* snapshot, uint32_t now_ms,
    bool* display_changed);
bool remote_link_usage_snapshot(struct remote_link_s* link,
    const struct remote_usage_snapshot_s* snapshot, bool* display_changed);
bool remote_link_timeout(struct remote_link_s* link, uint32_t now_ms);
/* `title` is the human-readable tool name and may be NULL: the bridge sends it
 * alongside the result, but an older bridge omits it. A raw ACP tool-call id
 * such as "tooluse_m7MqB5bPlKuDegoWjm6j38" tells the operator nothing, so it is
 * only a fallback when no title arrived. */
bool remote_link_tool_result(struct remote_link_s* link,
    const char* tool_call_id, const char* status, const char* title);

/* Records the model in use and the selectable list. Not authority: it changes
 * nothing about turns, permissions, or state. An entry that is too long, badly
 * encoded, or beyond REMOTE_MODEL_OPTIONS_MAX is skipped and the list is marked
 * truncated rather than rejecting the whole message. */
bool remote_link_models(struct remote_link_s* link,
    const struct remote_models_s* models);

/* ── Chat turns ────────────────────────────────────────────────────── */

bool remote_link_hello_ack(struct remote_link_s* link,
    const struct remote_hello_ack_s* ack);
/* Reserves the turn slot for `request_id` before the message is handed to a
 * transport, so a concurrent submit cannot open a second turn. */
bool remote_link_turn_submitted(struct remote_link_s* link,
    const char* request_id);

/* Releases a reservation whose send failed. Only clears the turn when it is
 * still the one identified by `request_id`, so a rollback can never discard a
 * turn that a later submit opened. */
bool remote_link_turn_abort(struct remote_link_s* link, const char* request_id);
bool remote_link_prompt_ack(struct remote_link_s* link,
    const struct remote_prompt_ack_s* ack);
bool remote_link_agent_output(struct remote_link_s* link,
    const struct remote_agent_output_s* output, bool* gap_detected);
bool remote_link_turn_result(struct remote_link_s* link,
    const struct remote_turn_result_s* result);

/* ── Outbound preconditions (codec serializes only what these allow) ─ */

/* True when a new chat turn may be submitted with this exact text. Rejects
 * while a turn is active or a permission is pending, so a local LLM tool call
 * can never queue behind a human decision. */
bool remote_link_can_submit_prompt(const struct remote_link_s* link,
    const char* text);

/* Returns "once" / "deny" when the pending prompt permits that decision,
 * NULL otherwise. */
const char* remote_link_decision_value(const struct remote_link_s* link,
    enum remote_decision_e decision);

/* True when a read-only usage query may be sent. */
bool remote_link_can_query_usage(const struct remote_link_s* link);

/* True when `model` may be requested. Refused while a turn is running: the
 * model that started a turn should be the one that finishes it. The list held
 * here is not consulted — it may be truncated or stale, and the bridge is the
 * only authority on what is selectable. */
bool remote_link_can_select_model(const struct remote_link_s* link,
    const char* model);

/* Console index resolution deliberately lives with the command that prints the
 * numbered list: remote_session_model_view returns the names and their order in
 * one consistent copy, so resolving a number against that same copy cannot pick
 * a different entry than the one displayed. */

/* ── Display helpers ───────────────────────────────────────────────── */

/* Renders "Billing: <charge> / <tokens>". Either slot may be empty; a missing
 * value is never substituted with zero and tokens are never priced. */
bool remote_link_billing_text(const struct remote_link_s* link, char* out,
    size_t out_size);
const char* remote_state_name(enum remote_state_e state);

/* Shared validators, also used by the codec layer. */
bool remote_valid_utf8(const char* text);
bool remote_valid_identifier(const char* value, size_t maximum);

#ifdef __cplusplus
}
#endif
