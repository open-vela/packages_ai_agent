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

#include "remote/remote_session.h"
#include "remote/remote_codec.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ── Internal helpers (all called with the lock held) ──────────────── */

/* Runs the presentation hook. Called under the lock, so the handler must not
 * re-enter the session. Handlers are expected to be cheap: anything that can
 * block for long delays the transport thread. */
static void emit(struct remote_session_s* session, enum remote_event_e event,
    const char* text)
{
    if (session->on_event != NULL) {
        session->on_event(session->event_context, event, text);
    }
}

static void clear_latch(struct remote_session_s* session)
{
    session->decision_in_flight = false;
    session->decided_prompt_id[0] = '\0';
}

static bool make_request_id(struct remote_session_s* session, char* out,
    size_t out_size)
{
    int written;

    session->next_request_number++;
    written = snprintf(out, out_size, "r-%.24s-%08" PRIx32,
        session->link.nonce, session->next_request_number);
    return written > 0 && (size_t)written < out_size;
}

/* Snapshots the active transport pointer so the send can happen after the lock
 * is released. The pointed-to transport outlives the session by construction
 * (it is owned by the transport module), and its send path re-checks its own
 * connection state, so a link that drops mid-send fails rather than misbehaves. */
static struct remote_transport_s* current_transport(
    struct remote_session_s* session)
{
    return session->active_transport;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void remote_session_init(struct remote_session_s* session, const char* nonce)
{
    /* memset would clear an initialised mutex, so the lock is created after it.
     * This must complete before any other thread can reach the session. */
    memset(session, 0, sizeof(*session));
    pthread_mutex_init(&session->lock, NULL);
    remote_link_init(&session->link, nonce);
}

void remote_session_destroy(struct remote_session_s* session)
{
    pthread_mutex_destroy(&session->lock);
}

void remote_session_set_event_handler(struct remote_session_s* session,
    remote_event_fn on_event, void* context)
{
    pthread_mutex_lock(&session->lock);
    session->on_event = on_event;
    session->event_context = context;
    pthread_mutex_unlock(&session->lock);
}

void remote_session_new_connection(struct remote_session_s* session,
    const char* nonce)
{
    pthread_mutex_lock(&session->lock);
    remote_link_init(&session->link, nonce);
    clear_latch(session);
    pthread_mutex_unlock(&session->lock);
}

void remote_session_set_active_transport(struct remote_session_s* session,
    struct remote_transport_s* transport)
{
    pthread_mutex_lock(&session->lock);
    session->active_transport = transport;
    pthread_mutex_unlock(&session->lock);
}

void remote_session_link_down(struct remote_session_s* session)
{
    pthread_mutex_lock(&session->lock);
    remote_link_down(&session->link);
    clear_latch(session);
    pthread_mutex_unlock(&session->lock);
}

/* ── Inbound ───────────────────────────────────────────────────────── */

bool remote_session_snapshot(struct remote_session_s* session,
    const struct remote_snapshot_s* snapshot, uint32_t now_ms,
    bool* display_changed)
{
    bool changed = false;
    bool accepted;

    pthread_mutex_lock(&session->lock);
    accepted = remote_link_snapshot(&session->link, snapshot, now_ms, &changed);
    if (accepted) {
        /* A later authoritative snapshot is the only thing that clears the
         * latch: either the prompt is gone or it was replaced. */
        if (!session->link.has_prompt || strcmp(session->link.prompt.id, session->decided_prompt_id) != 0) {
            clear_latch(session);
        }

        if (changed) {
            emit(session, REMOTE_EVENT_STATE, session->link.summary);
        }
    }
    pthread_mutex_unlock(&session->lock);

    if (display_changed != NULL) {
        *display_changed = changed;
    }

    return accepted;
}

bool remote_session_usage_snapshot(struct remote_session_s* session,
    const struct remote_usage_snapshot_s* snapshot, bool* display_changed)
{
    char text[96];
    bool changed = false;
    bool accepted;

    pthread_mutex_lock(&session->lock);
    accepted = remote_link_usage_snapshot(&session->link, snapshot, &changed);
    if (accepted && changed && remote_link_billing_text(&session->link, text, sizeof(text))) {
        emit(session, REMOTE_EVENT_BILLING, text);
    }
    pthread_mutex_unlock(&session->lock);

    if (display_changed != NULL) {
        *display_changed = changed;
    }

    return accepted;
}

bool remote_session_tool_result(struct remote_session_s* session,
    const char* connection_nonce, const char* epoch, const char* tool_call_id,
    const char* status, const char* title)
{
    bool accepted = false;

    if (session == NULL || connection_nonce == NULL || epoch == NULL) {
        return false;
    }

    pthread_mutex_lock(&session->lock);
    /* Session binding is checked here rather than by the caller so no reader
     * touches link state without the lock. */
    if (strcmp(connection_nonce, session->link.nonce) == 0 && strcmp(epoch, session->link.epoch) == 0) {
        accepted = remote_link_tool_result(&session->link, tool_call_id, status,
            title);
    }

    if (accepted) {
        clear_latch(session);
        emit(session, REMOTE_EVENT_STATE, session->link.summary);
    }
    pthread_mutex_unlock(&session->lock);

    return accepted;
}

bool remote_session_timeout(struct remote_session_s* session, uint32_t now_ms)
{
    bool expired;

    pthread_mutex_lock(&session->lock);
    expired = remote_link_timeout(&session->link, now_ms);
    if (expired) {
        clear_latch(session);
        emit(session, REMOTE_EVENT_STATE, session->link.summary);
    }
    pthread_mutex_unlock(&session->lock);

    return expired;
}

bool remote_session_hello_ack(struct remote_session_s* session,
    const struct remote_hello_ack_s* ack)
{
    bool accepted;

    if (session == NULL) {
        return false;
    }

    pthread_mutex_lock(&session->lock);
    accepted = remote_link_hello_ack(&session->link, ack);
    pthread_mutex_unlock(&session->lock);

    return accepted;
}

bool remote_session_prompt_ack(struct remote_session_s* session,
    const struct remote_prompt_ack_s* ack)
{
    bool accepted;

    if (session == NULL || ack == NULL) {
        return false;
    }

    pthread_mutex_lock(&session->lock);
    accepted = remote_link_prompt_ack(&session->link, ack);
    if (accepted && !ack->accepted) {
        emit(session, REMOTE_EVENT_PROMPT_REJECTED, NULL);
    }
    pthread_mutex_unlock(&session->lock);

    return accepted;
}

bool remote_session_agent_output(struct remote_session_s* session,
    const struct remote_agent_output_s* output, bool* gap_detected)
{
    bool gap = false;
    bool accepted;

    if (session == NULL || output == NULL) {
        return false;
    }

    pthread_mutex_lock(&session->lock);
    accepted = remote_link_agent_output(&session->link, output, &gap);
    if (accepted) {
        if (gap) {
            emit(session, REMOTE_EVENT_OUTPUT_GAP, NULL);
        }
        emit(session, REMOTE_EVENT_AGENT_OUTPUT, output->text);
    }
    pthread_mutex_unlock(&session->lock);

    if (gap_detected != NULL) {
        *gap_detected = gap;
    }

    return accepted;
}

bool remote_session_models(struct remote_session_s* session,
    const struct remote_models_s* models)
{
    char previous[REMOTE_MODEL_MAX + 1];
    bool accepted;

    if (session == NULL || models == NULL) {
        return false;
    }

    pthread_mutex_lock(&session->lock);
    strcpy(previous, session->link.model);
    accepted = remote_link_models(&session->link, models);
    /* Only a real change is announced. The bridge repeats this message on every
     * handshake, and echoing an unchanged model each time would be noise. */
    if (accepted && strcmp(previous, session->link.model) != 0) {
        emit(session, REMOTE_EVENT_MODEL, session->link.model);
    }
    pthread_mutex_unlock(&session->lock);

    return accepted;
}

bool remote_session_turn_result(struct remote_session_s* session,
    const struct remote_turn_result_s* result)
{
    bool accepted;

    if (session == NULL) {
        return false;
    }

    pthread_mutex_lock(&session->lock);
    accepted = remote_link_turn_result(&session->link, result);
    if (accepted) {
        /* Transcript before the outcome, so the reply reads above the line that
         * says the turn ended. remote_link_turn_result keeps the transcript, so
         * it is still intact here. Skipped when empty, which is what a turn that
         * only ran tools produces. */
        if (session->link.transcript[0] != '\0') {
            emit(session, REMOTE_EVENT_TURN_TRANSCRIPT, session->link.transcript);
        }
        emit(session, REMOTE_EVENT_TURN_RESULT, session->link.summary);
    }
    pthread_mutex_unlock(&session->lock);

    return accepted;
}

/* ── Outbound ──────────────────────────────────────────────────────── */

/* Every path below follows the same shape: serialize under the lock, release,
 * then send. Holding the lock across a transport call would invert lock order
 * against the inbound path, where MQTT-C already holds its client mutex when it
 * delivers a message. */

bool remote_session_send_hello(struct remote_session_s* session)
{
    char message[REMOTE_CODEC_HELLO_BUF];
    struct remote_transport_s* transport;
    bool built;

    if (session == NULL) {
        return false;
    }

    pthread_mutex_lock(&session->lock);
    built = remote_codec_build_hello(&session->link, message, sizeof(message));
    transport = current_transport(session);
    pthread_mutex_unlock(&session->lock);

    return built && remote_transport_send(transport, message, strlen(message));
}

bool remote_session_decide(struct remote_session_s* session,
    enum remote_decision_e decision)
{
    char message[REMOTE_CODEC_DECISION_BUF];
    char prompt_id[REMOTE_PROMPT_ID_MAX + 1];
    struct remote_transport_s* transport;
    bool built = false;

    if (session == NULL) {
        return false;
    }

    pthread_mutex_lock(&session->lock);
    /* One decision per prompt. Repeat local input is refused here; the bridge
     * independently rejects duplicate, stale, or mismatched decisions. */
    if (!session->decision_in_flight) {
        built = remote_codec_build_decision(&session->link, decision, message, sizeof(message));
    }

    if (built) {
        /* Claim the latch before releasing the lock so a second decision
         * cannot be built for the same prompt while this one is in flight. */
        strcpy(session->decided_prompt_id, session->link.prompt.id);
        strcpy(prompt_id, session->link.prompt.id);
        session->decision_in_flight = true;
    }
    transport = current_transport(session);
    pthread_mutex_unlock(&session->lock);

    if (!built) {
        return false;
    }

    if (remote_transport_send(transport, message, strlen(message))) {
        return true;
    }

    /* The decision never left the device, so release the claim — but only if
     * it is still the one made above. */
    pthread_mutex_lock(&session->lock);
    if (session->decision_in_flight && strcmp(session->decided_prompt_id, prompt_id) == 0) {
        clear_latch(session);
    }
    pthread_mutex_unlock(&session->lock);

    return false;
}

bool remote_session_select_model(struct remote_session_s* session,
    const char* model)
{
    char message[REMOTE_CODEC_MODEL_SELECT_BUF];
    struct remote_transport_s* transport;
    bool built;

    if (session == NULL) {
        return false;
    }

    pthread_mutex_lock(&session->lock);
    built = remote_codec_build_model_select(&session->link, model, message,
        sizeof(message));
    transport = current_transport(session);
    pthread_mutex_unlock(&session->lock);

    /* Nothing local to roll back: the model in use belongs to the bridge, and
     * this device only ever learns it from a `models` message. A send that
     * fails simply leaves the previously reported model in place. */
    return built && remote_transport_send(transport, message, strlen(message));
}

bool remote_session_query_usage(struct remote_session_s* session)
{
    char message[REMOTE_CODEC_USAGE_QUERY_BUF];
    struct remote_transport_s* transport;
    bool built;

    if (session == NULL) {
        return false;
    }

    pthread_mutex_lock(&session->lock);
    built = remote_codec_build_usage_query(&session->link, message, sizeof(message));
    transport = current_transport(session);
    pthread_mutex_unlock(&session->lock);

    /* Read-only: nothing to roll back if the send fails. */
    return built && remote_transport_send(transport, message, strlen(message));
}

bool remote_session_submit_prompt(struct remote_session_s* session,
    const char* text)
{
    char request_id[REMOTE_REQUEST_ID_MAX + 1];
    struct remote_transport_s* transport;
    size_t length = 0;
    bool reserved = false;

    if (session == NULL) {
        return false;
    }

    pthread_mutex_lock(&session->lock);
    /* The precondition is checked before minting an id so a submit refused
     * locally does not consume one. A submit that fails later, at the transport,
     * keeps its id burnt on purpose: the message may already have reached the
     * bridge, and reusing the id could conflate two turns. */
    if (remote_link_can_submit_prompt(&session->link, text) && make_request_id(session, request_id, sizeof(request_id)) && remote_codec_build_prompt_submit(&session->link, request_id, text, session->prompt_message, sizeof(session->prompt_message))) {
        /* Reserve the turn while still holding the lock: the reservation is
         * what makes a concurrent submit fail instead of opening a second turn,
         * and it also protects the shared staging buffer. */
        reserved = remote_link_turn_submitted(&session->link, request_id);
        length = strlen(session->prompt_message);
    }
    transport = current_transport(session);
    pthread_mutex_unlock(&session->lock);

    if (!reserved) {
        return false;
    }

    /* Safe to read the staging buffer with the lock released: the reservation
     * above set turn_active, and remote_link_can_submit_prompt refuses while
     * that holds, so no concurrent submit can reach the encoder. */
    if (remote_transport_send(transport, session->prompt_message, length)) {
        return true;
    }

    /* The turn never left the device, so free the slot again. Aborting is
     * request-id scoped and does nothing if an ack already closed the turn. */
    pthread_mutex_lock(&session->lock);
    remote_link_turn_abort(&session->link, request_id);
    pthread_mutex_unlock(&session->lock);
    return false;
}

/* ── Status ────────────────────────────────────────────────────────── */

void remote_session_status(struct remote_session_s* session,
    struct remote_status_s* out)
{
    memset(out, 0, sizeof(*out));

    pthread_mutex_lock(&session->lock);
    out->state = session->link.state;
    memcpy(out->summary, session->link.summary, sizeof(out->summary));
    out->has_prompt = session->link.has_prompt;
    out->prompt = session->link.prompt;
    out->decision_in_flight = session->decision_in_flight;
    out->turn_active = session->link.turn_active;
    out->chat_supported = session->link.chat_supported;
    strcpy(out->model, session->link.model);
    if (!remote_link_billing_text(&session->link, out->billing, sizeof(out->billing))) {
        out->billing[0] = '\0';
    }
    pthread_mutex_unlock(&session->lock);
}

bool remote_session_transcript(struct remote_session_s* session, char* out,
    size_t out_size)
{
    bool fits;

    if (session == NULL || out == NULL || out_size == 0) {
        return false;
    }

    pthread_mutex_lock(&session->lock);
    fits = strlen(session->link.transcript) < out_size;
    if (fits) {
        strcpy(out, session->link.transcript);
    } else {
        out[0] = '\0';
    }
    pthread_mutex_unlock(&session->lock);

    return fits;
}

void remote_session_model_view(struct remote_session_s* session,
    struct remote_model_view_s* out)
{
    if (out == NULL) {
        return;
    }

    memset(out, 0, sizeof(*out));
    if (session == NULL) {
        return;
    }

    /* Copied in one pass under the lock: a listing that read the names one at a
     * time could show entries from two different bridge updates and number them
     * inconsistently with what a following select would resolve. */
    pthread_mutex_lock(&session->lock);
    strcpy(out->current, session->link.model);
    memcpy(out->options, session->link.model_options, sizeof(out->options));
    out->count = session->link.model_count;
    out->truncated = session->link.models_truncated;
    out->turn_active = session->link.turn_active;
    pthread_mutex_unlock(&session->lock);
}
