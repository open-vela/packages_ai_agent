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

/* Remote-control session: owns one protocol link plus its active transport,
 * and holds the local decision latch. Everything that leaves the device goes
 * through here.
 *
 * Concurrency. A session is reached from several threads: the transport thread
 * applies inbound messages, while NSH commands and tool calls submit outbound
 * ones. Every function here locks internally, so callers need no lock of their
 * own. Two rules keep that safe:
 *
 *   1. The transport is never called with the session lock held. MQTT-C runs
 *      the inbound callback while holding its own client mutex, so taking the
 *      session lock inside a publish and the MQTT mutex inside a send would
 *      invert lock order between the two paths. Outbound calls therefore build
 *      the message under the lock, release it, and only then send.
 *   2. An event handler runs with the lock held and must not call back into
 *      any remote_session_* function.
 */

#pragma once

#include "remote/remote_link.h"
#include "remote/remote_transport.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum remote_event_e {
    REMOTE_EVENT_STATE, /* state or summary changed */
    REMOTE_EVENT_AGENT_OUTPUT, /* one chunk of remote agent text */
    REMOTE_EVENT_OUTPUT_GAP, /* QoS 0 loss noted, authority unchanged */
    /* The whole reply, once, when the turn ends. Chunks arrive too small and too
     * often to read as they stream, so the link accumulates them and the reply is
     * presented as one piece here. Carries the transcript itself, so a handler
     * never has to call back into the session to fetch it. */
    REMOTE_EVENT_TURN_TRANSCRIPT,
    /* The remote agent's model changed. Emitted only on a real change, so a
     * switch made on the PC side is visible here without polling. */
    REMOTE_EVENT_MODEL,
    REMOTE_EVENT_TURN_RESULT,
    REMOTE_EVENT_PROMPT_REJECTED,
    REMOTE_EVENT_BILLING,
};

/* Presentation hook. The session never prints: the channel decides whether an
 * event goes to syslog, the message bus, or a UI. Invoked with the session
 * lock held, so it must not re-enter the session. */
typedef void (*remote_event_fn)(void* context, enum remote_event_e event,
    const char* text);

/* Consistent view of the session for display, copied out under the lock. */
struct remote_status_s {
    enum remote_state_e state;
    char summary[REMOTE_SUMMARY_MAX + 1];
    char billing[96];
    bool has_prompt;
    struct remote_prompt_s prompt;
    bool decision_in_flight;
    bool turn_active;
    bool chat_supported;
    /* Empty until the bridge reports one. The selectable list is deliberately
     * not here: it is far larger than the rest of this struct and only one
     * command needs it, so it is fetched separately. */
    char model[REMOTE_MODEL_MAX + 1];
};

/* The model list, copied out under the lock in one go so a console listing
 * cannot interleave with a bridge update. Too large for the stack on this
 * target — declare it static at the call site. */
struct remote_model_view_s {
    char current[REMOTE_MODEL_MAX + 1];
    char options[REMOTE_MODEL_OPTIONS_MAX][REMOTE_MODEL_MAX + 1];
    uint8_t count;
    bool truncated;
    bool turn_active;
};

struct remote_session_s {
    struct remote_link_s link;
    struct remote_transport_s* active_transport;
    char decided_prompt_id[REMOTE_PROMPT_ID_MAX + 1];
    bool decision_in_flight;
    uint32_t next_request_number;
    remote_event_fn on_event;
    void* event_context;
    pthread_mutex_t lock;
    /* Staging buffer for an outbound prompt. Sized to the encoded limit rather
     * than the text limit, because escaping can expand a prompt several times
     * over. Serialised by the turn reservation: a second submit is refused
     * while a turn is in flight. */
    char prompt_message[REMOTE_MESSAGE_MAX + 1];
};

/* ── Lifecycle ─────────────────────────────────────────────────────── */

/* Must complete before any other thread can reach the session. */
void remote_session_init(struct remote_session_s* session, const char* nonce);
void remote_session_destroy(struct remote_session_s* session);
void remote_session_set_event_handler(struct remote_session_s* session,
    remote_event_fn on_event, void* context);
void remote_session_new_connection(struct remote_session_s* session,
    const char* nonce);
void remote_session_set_active_transport(struct remote_session_s* session,
    struct remote_transport_s* transport);
void remote_session_link_down(struct remote_session_s* session);

/* ── Inbound (called by the codec) ─────────────────────────────────── */

bool remote_session_snapshot(struct remote_session_s* session,
    const struct remote_snapshot_s* snapshot, uint32_t now_ms,
    bool* display_changed);
bool remote_session_usage_snapshot(struct remote_session_s* session,
    const struct remote_usage_snapshot_s* snapshot, bool* display_changed);
/* Session binding is validated inside, so the codec never reads link state
 * without the lock. */
/* `title` may be NULL: see remote_link_tool_result. */
bool remote_session_tool_result(struct remote_session_s* session,
    const char* connection_nonce, const char* epoch, const char* tool_call_id,
    const char* status, const char* title);
bool remote_session_timeout(struct remote_session_s* session, uint32_t now_ms);
bool remote_session_hello_ack(struct remote_session_s* session,
    const struct remote_hello_ack_s* ack);
bool remote_session_prompt_ack(struct remote_session_s* session,
    const struct remote_prompt_ack_s* ack);
bool remote_session_agent_output(struct remote_session_s* session,
    const struct remote_agent_output_s* output, bool* gap_detected);
bool remote_session_turn_result(struct remote_session_s* session,
    const struct remote_turn_result_s* result);
bool remote_session_models(struct remote_session_s* session,
    const struct remote_models_s* models);

/* ── Outbound ──────────────────────────────────────────────────────── */

/* Sends the connection handshake for the session's current nonce. */
bool remote_session_send_hello(struct remote_session_s* session);

/* Permission decision. HUMAN INPUT ONLY — reached from NSH commands and, in
 * future, a GPIO key. This must never be exposed as an LLM-callable tool:
 * approving a remote agent's tool call is a human act, and a model driving
 * both sides could approve its own request. */
bool remote_session_decide(struct remote_session_s* session,
    enum remote_decision_e decision);

/* Read-only billing refresh. Safe for tools: cannot alter authority state. */
bool remote_session_query_usage(struct remote_session_s* session);

/* Asks the bridge to switch the remote agent's model for the rest of the
 * session. Refused while a turn is running. Sending only requests the change:
 * the bridge validates the name and reports the result in a `models` message,
 * so a success here means the request left the device, not that it was applied. */
bool remote_session_select_model(struct remote_session_s* session,
    const char* model);

/* Submits one chat turn to the remote agent. Safe for tools: rejects locally
 * while a turn or a permission request is active, so it fails fast instead of
 * blocking a local agent loop behind a human decision. */
bool remote_session_submit_prompt(struct remote_session_s* session,
    const char* text);

/* ── Status ────────────────────────────────────────────────────────── */

void remote_session_status(struct remote_session_s* session,
    struct remote_status_s* out);

/* Copies the bounded agent transcript. */
bool remote_session_transcript(struct remote_session_s* session, char* out,
    size_t out_size);

/* Copies the model list for display. `out` is large; see remote_model_view_s. */
void remote_session_model_view(struct remote_session_s* session,
    struct remote_model_view_s* out);

#ifdef __cplusplus
}
#endif
