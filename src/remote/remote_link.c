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

#include "remote/remote_link.h"

#include <float.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* ── Local helpers ─────────────────────────────────────────────────── */

static void copy_bounded(char* out, size_t out_size, const char* value)
{
    if (out_size == 0) {
        return;
    }

    if (value == NULL) {
        out[0] = '\0';
        return;
    }

    strncpy(out, value, out_size - 1);
    out[out_size - 1] = '\0';
}

/* A summary is rendered as a single status line, so a control character inside
 * it — a newline in a tool title being the common case — would split that line
 * in two and make a status line read like a truncated transcript. Only ASCII
 * control bytes are replaced; anything >= 0x80 is left alone so multi-byte UTF-8
 * survives intact. */
static void flatten_summary(char* text)
{
    size_t i;

    if (text == NULL) {
        return;
    }

    for (i = 0; text[i] != '\0'; i++) {
        unsigned char byte = (unsigned char)text[i];

        if (byte < 0x20 || byte == 0x7f) {
            text[i] = ' ';
        }
    }
}

static void clear_prompt(struct remote_link_s* link)
{
    memset(&link->prompt, 0, sizeof(link->prompt));
    link->has_prompt = false;
}

/* Offline means the model in use is unknown, not "still the last one seen": the
 * bridge may be restarted against a different session before we hear again. */
static void clear_models(struct remote_link_s* link)
{
    link->model[0] = '\0';
    link->model_count = 0;
    link->models_truncated = false;
}

static void clear_usage(struct remote_link_s* link)
{
    memset(&link->usage, 0, sizeof(link->usage));
}

static void clear_turn(struct remote_link_s* link, bool clear_transcript)
{
    link->turn_active = false;
    link->turn_request_id[0] = '\0';
    link->turn_id[0] = '\0';
    link->last_output_seq = 0;
    link->output_gap = false;
    if (clear_transcript) {
        link->transcript[0] = '\0';
    }
}

bool remote_valid_utf8(const char* text)
{
    const unsigned char* cursor = (const unsigned char*)text;

    if (text == NULL) {
        return false;
    }

    while (*cursor != '\0') {
        if (*cursor < 0x80) {
            cursor++;
        } else if (*cursor >= 0xc2 && *cursor <= 0xdf && cursor[1] != '\0' && (cursor[1] & 0xc0) == 0x80) {
            cursor += 2;
        } else if (*cursor >= 0xe0 && *cursor <= 0xef && cursor[1] != '\0' && cursor[2] != '\0' && (cursor[1] & 0xc0) == 0x80 && (cursor[2] & 0xc0) == 0x80 && !(*cursor == 0xe0 && cursor[1] < 0xa0) && !(*cursor == 0xed && cursor[1] >= 0xa0)) {
            cursor += 3;
        } else if (*cursor >= 0xf0 && *cursor <= 0xf4 && cursor[1] != '\0' && cursor[2] != '\0' && cursor[3] != '\0' && (cursor[1] & 0xc0) == 0x80 && (cursor[2] & 0xc0) == 0x80 && (cursor[3] & 0xc0) == 0x80 && !(*cursor == 0xf0 && cursor[1] < 0x90) && !(*cursor == 0xf4 && cursor[1] >= 0x90)) {
            cursor += 4;
        } else {
            return false;
        }
    }

    return true;
}

bool remote_valid_identifier(const char* value, size_t maximum)
{
    const unsigned char* cursor = (const unsigned char*)value;

    if (value == NULL || value[0] == '\0' || strlen(value) > maximum || !remote_valid_utf8(value)) {
        return false;
    }

    while (*cursor != '\0') {
        if (*cursor < 0x20 || *cursor == 0x7f) {
            return false;
        }
        cursor++;
    }

    return true;
}

/* Agent text may contain newlines but no other control characters. */
static bool valid_agent_text(const char* value)
{
    const unsigned char* cursor = (const unsigned char*)value;

    if (value == NULL || value[0] == '\0' || strlen(value) > REMOTE_OUTPUT_TEXT_MAX || !remote_valid_utf8(value)) {
        return false;
    }

    while (*cursor != '\0') {
        if (*cursor < 0x20 && *cursor != '\n') {
            return false;
        }
        cursor++;
    }

    return true;
}

static bool has_nonspace(const char* value)
{
    while (value != NULL && *value != '\0') {
        if (*value != ' ' && *value != '\t' && *value != '\n' && *value != '\r') {
            return true;
        }
        value++;
    }
    return false;
}

/* Keeps a bounded tail of agent output. Drops whole UTF-8 sequences so the
 * retained transcript never starts mid-character. */
static void append_transcript(struct remote_link_s* link, const char* text)
{
    size_t existing = strlen(link->transcript);
    size_t added = strlen(text);
    size_t drop = existing + added > REMOTE_TRANSCRIPT_MAX
        ? existing + added - REMOTE_TRANSCRIPT_MAX
        : 0;

    if (drop >= existing) {
        size_t start = added - (REMOTE_TRANSCRIPT_MAX < added ? REMOTE_TRANSCRIPT_MAX : added);

        while (text[start] != '\0' && ((unsigned char)text[start] & 0xc0) == 0x80) {
            start++;
        }
        copy_bounded(link->transcript, sizeof(link->transcript), text + start);
        return;
    }

    if (drop > 0) {
        while (drop < existing && ((unsigned char)link->transcript[drop] & 0xc0) == 0x80) {
            drop++;
        }
        memmove(link->transcript, link->transcript + drop, existing - drop + 1);
    }

    strncat(link->transcript, text, sizeof(link->transcript) - strlen(link->transcript) - 1);
}

static bool same_prompt(const struct remote_prompt_s* left,
    const struct remote_prompt_s* right)
{
    return strcmp(left->id, right->id) == 0 && strcmp(left->tool, right->tool) == 0 && strcmp(left->hint, right->hint) == 0 && left->can_once == right->can_once && left->can_deny == right->can_deny;
}

static bool valid_nonnegative_double(double value)
{
    return value == value && value >= 0 && value <= DBL_MAX;
}

static bool same_usage(const struct remote_usage_s* left,
    const struct remote_usage_snapshot_s* right)
{
    return left->has_charge == right->has_charge && (!left->has_charge || (left->charge_kind == right->charge_kind && left->charge_amount == right->charge_amount && strcmp(left->charge_unit, right->charge_unit) == 0)) && left->has_tokens == right->has_tokens && (!left->has_tokens || (left->input_tokens == right->input_tokens && left->output_tokens == right->output_tokens && left->total_tokens == right->total_tokens)) && left->has_context == right->has_context && (!left->has_context || (left->context_unit == right->context_unit && left->context_used == right->context_used && left->context_limit == right->context_limit));
}

static bool valid_usage_snapshot(const struct remote_link_s* link,
    const struct remote_usage_snapshot_s* snapshot)
{
    if (snapshot == NULL || snapshot->connection_nonce == NULL || snapshot->epoch == NULL || link->epoch[0] == '\0' || !remote_valid_identifier(snapshot->connection_nonce, REMOTE_NONCE_MAX) || !remote_valid_identifier(snapshot->epoch, REMOTE_EPOCH_MAX) || strcmp(snapshot->connection_nonce, link->nonce) != 0 || strcmp(snapshot->epoch, link->epoch) != 0 || snapshot->seq == 0 || snapshot->seq <= link->usage.last_seq) {
        return false;
    }

    if (snapshot->has_charge && ((snapshot->charge_kind != REMOTE_CHARGE_CREDIT && snapshot->charge_kind != REMOTE_CHARGE_CURRENCY) || snapshot->charge_unit == NULL || strlen(snapshot->charge_unit) > REMOTE_BILLING_UNIT_MAX || !valid_nonnegative_double(snapshot->charge_amount))) {
        return false;
    }

    return !snapshot->has_context || ((snapshot->context_unit == REMOTE_CONTEXT_TOKEN || snapshot->context_unit == REMOTE_CONTEXT_PERCENT) && valid_nonnegative_double(snapshot->context_used) && valid_nonnegative_double(snapshot->context_limit) && snapshot->context_limit > 0);
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void remote_link_init(struct remote_link_s* link, const char* nonce)
{
    memset(link, 0, sizeof(*link));
    copy_bounded(link->nonce, sizeof(link->nonce), nonce);
    link->state = REMOTE_STATE_OFFLINE;
}

void remote_link_down(struct remote_link_s* link)
{
    link->epoch[0] = '\0';
    link->next_seq = 0;
    link->last_snapshot_ms = 0;
    link->summary[0] = '\0';
    clear_prompt(link);
    clear_usage(link);
    clear_models(link);
    link->chat_epoch[0] = '\0';
    link->chat_supported = false;
    clear_turn(link, true);
    link->state = REMOTE_STATE_OFFLINE;
}

/* ── Inbound authority ─────────────────────────────────────────────── */

bool remote_link_snapshot(struct remote_link_s* link,
    const struct remote_snapshot_s* snapshot, uint32_t now_ms,
    bool* display_changed)
{
    enum remote_state_e next_state;
    bool next_has_prompt;
    bool changed;
    char next_summary[REMOTE_SUMMARY_MAX + 1];

    if (display_changed != NULL) {
        *display_changed = false;
    }

    if (snapshot == NULL || snapshot->connection_nonce == NULL || snapshot->epoch == NULL || snapshot->summary == NULL || strlen(snapshot->summary) > REMOTE_SUMMARY_MAX || !remote_valid_identifier(snapshot->connection_nonce, REMOTE_NONCE_MAX) || !remote_valid_identifier(snapshot->epoch, REMOTE_EPOCH_MAX) || (snapshot->prompt != NULL && !remote_valid_identifier(snapshot->prompt->id, REMOTE_PROMPT_ID_MAX)) || strcmp(snapshot->connection_nonce, link->nonce) != 0) {
        return false;
    }

    /* First snapshot of a connection locks the epoch and the sequence base. */
    if (link->epoch[0] == '\0') {
        copy_bounded(link->epoch, sizeof(link->epoch), snapshot->epoch);
        link->next_seq = 1;
        link->usage.last_seq = 0;
        if (strcmp(link->chat_epoch, link->epoch) != 0) {
            link->chat_supported = false;
        }
    }

    if (strcmp(snapshot->epoch, link->epoch) != 0 || snapshot->seq != link->next_seq) {
        return false;
    }

    next_has_prompt = snapshot->prompt != NULL;
    if (next_has_prompt) {
        next_state = REMOTE_STATE_ATTENTION;
    } else if (snapshot->running > 0) {
        next_state = REMOTE_STATE_BUSY;
    } else {
        next_state = REMOTE_STATE_IDLE;
    }

    /* Flatten before comparing, never after storing: the stored summary is the
     * flattened form, so comparing it against the raw incoming one would differ
     * on every snapshot whose summary carries a control byte, report a change
     * each time, and turn a quiet link into a stream of identical status lines. */
    copy_bounded(next_summary, sizeof(next_summary), snapshot->summary);
    flatten_summary(next_summary);

    changed = link->state != next_state || strcmp(link->summary, next_summary) != 0 || link->has_prompt != next_has_prompt || (next_has_prompt && !same_prompt(&link->prompt, snapshot->prompt));

    link->next_seq++;
    link->last_snapshot_ms = now_ms;
    copy_bounded(link->summary, sizeof(link->summary), next_summary);
    if (!next_has_prompt) {
        clear_prompt(link);
    } else {
        link->prompt = *snapshot->prompt;
        link->prompt.id[REMOTE_PROMPT_ID_MAX] = '\0';
        link->prompt.tool[REMOTE_TOOL_MAX] = '\0';
        link->prompt.hint[REMOTE_HINT_MAX] = '\0';
        link->has_prompt = true;
    }
    link->state = next_state;

    if (display_changed != NULL) {
        *display_changed = changed;
    }

    return true;
}

bool remote_link_timeout(struct remote_link_s* link, uint32_t now_ms)
{
    if (link->state == REMOTE_STATE_OFFLINE || now_ms - link->last_snapshot_ms <= REMOTE_SNAPSHOT_TIMEOUT_MS) {
        return false;
    }

    remote_link_down(link);
    return true;
}

bool remote_link_tool_result(struct remote_link_s* link,
    const char* tool_call_id, const char* status, const char* title)
{
    const char* label;

    if (link == NULL || tool_call_id == NULL || status == NULL || (strcmp(status, "completed") != 0 && strcmp(status, "failed") != 0)) {
        return false;
    }

    /* A tool result is not authority to clear a still-pending permission. Tool
     * calls that did not request device permission can complete at any time. */
    if (link->has_prompt) {
        return false;
    }

    /* Prefer the name a human can read. The id stays as the fallback so an older
     * bridge that sends no title still produces a usable line. */
    label = (title != NULL && title[0] != '\0') ? title : tool_call_id;
    snprintf(link->summary, sizeof(link->summary), "tool '%s' %s", label, status);
    flatten_summary(link->summary);
    clear_prompt(link);
    link->state = REMOTE_STATE_RESULT;
    return true;
}

bool remote_link_usage_snapshot(struct remote_link_s* link,
    const struct remote_usage_snapshot_s* snapshot, bool* display_changed)
{
    bool changed;

    if (display_changed != NULL) {
        *display_changed = false;
    }

    if (!valid_usage_snapshot(link, snapshot)) {
        return false;
    }

    changed = !same_usage(&link->usage, snapshot);
    link->usage.last_seq = snapshot->seq;
    link->usage.has_charge = snapshot->has_charge;
    link->usage.charge_kind = snapshot->charge_kind;
    link->usage.charge_amount = snapshot->charge_amount;
    copy_bounded(link->usage.charge_unit, sizeof(link->usage.charge_unit),
        snapshot->has_charge ? snapshot->charge_unit : "");
    link->usage.has_tokens = snapshot->has_tokens;
    link->usage.input_tokens = snapshot->input_tokens;
    link->usage.output_tokens = snapshot->output_tokens;
    link->usage.total_tokens = snapshot->total_tokens;
    link->usage.has_context = snapshot->has_context;
    link->usage.context_unit = snapshot->context_unit;
    link->usage.context_used = snapshot->context_used;
    link->usage.context_limit = snapshot->context_limit;

    if (display_changed != NULL) {
        *display_changed = changed;
    }

    return true;
}

bool remote_link_models(struct remote_link_s* link,
    const struct remote_models_s* models)
{
    size_t i;
    uint8_t stored = 0;
    bool dropped = false;

    if (link == NULL || models == NULL || models->connection_nonce == NULL || models->epoch == NULL || link->epoch[0] == '\0' || !remote_valid_identifier(models->connection_nonce, REMOTE_NONCE_MAX) || !remote_valid_identifier(models->epoch, REMOTE_EPOCH_MAX) || strcmp(models->connection_nonce, link->nonce) != 0 || strcmp(models->epoch, link->epoch) != 0 || !remote_valid_identifier(models->current, REMOTE_MODEL_MAX)) {
        return false;
    }

    /* An unusable entry is skipped rather than failing the message: the current
     * model is the part that matters, and a partial list still lets most of the
     * numbered choices work. Whatever was dropped is reported as truncation. */
    for (i = 0; i < models->count && models->options != NULL; i++) {
        const char* option = models->options[i];

        if (!remote_valid_identifier(option, REMOTE_MODEL_MAX)) {
            dropped = true;
            continue;
        }

        if (stored >= REMOTE_MODEL_OPTIONS_MAX) {
            dropped = true;
            break;
        }

        copy_bounded(link->model_options[stored], REMOTE_MODEL_MAX + 1, option);
        stored++;
    }

    copy_bounded(link->model, sizeof(link->model), models->current);
    link->model_count = stored;
    link->models_truncated = models->truncated || dropped;
    return true;
}

/* ── Chat turns ────────────────────────────────────────────────────── */

bool remote_link_hello_ack(struct remote_link_s* link,
    const struct remote_hello_ack_s* ack)
{
    if (link == NULL || ack == NULL || ack->connection_nonce == NULL || ack->epoch == NULL || strcmp(ack->connection_nonce, link->nonce) != 0 || !remote_valid_identifier(ack->connection_nonce, REMOTE_NONCE_MAX) || !remote_valid_identifier(ack->epoch, REMOTE_EPOCH_MAX)) {
        return false;
    }

    copy_bounded(link->chat_epoch, sizeof(link->chat_epoch), ack->epoch);
    link->chat_supported = ack->chat_turns;
    return true;
}

bool remote_link_turn_submitted(struct remote_link_s* link,
    const char* request_id)
{
    if (link == NULL || link->turn_active || !remote_valid_identifier(request_id, REMOTE_REQUEST_ID_MAX)) {
        return false;
    }

    clear_turn(link, true);
    copy_bounded(link->turn_request_id, sizeof(link->turn_request_id), request_id);
    link->turn_active = true;
    return true;
}

bool remote_link_turn_abort(struct remote_link_s* link, const char* request_id)
{
    if (link == NULL || !link->turn_active || request_id == NULL || strcmp(link->turn_request_id, request_id) != 0) {
        return false;
    }

    /* Keeps the transcript: a send that never left the device produced no
     * agent output of its own. */
    clear_turn(link, false);
    return true;
}

bool remote_link_prompt_ack(struct remote_link_s* link,
    const struct remote_prompt_ack_s* ack)
{
    if (link == NULL || ack == NULL || ack->connection_nonce == NULL || ack->epoch == NULL || !remote_valid_identifier(ack->request_id, REMOTE_REQUEST_ID_MAX) || !remote_valid_identifier(ack->connection_nonce, REMOTE_NONCE_MAX) || !remote_valid_identifier(ack->epoch, REMOTE_EPOCH_MAX) || strcmp(ack->connection_nonce, link->nonce) != 0 || strcmp(ack->epoch, link->epoch) != 0 || !link->turn_active || strcmp(ack->request_id, link->turn_request_id) != 0) {
        return false;
    }

    if (!ack->accepted) {
        clear_turn(link, false);
        return true;
    }

    if (!remote_valid_identifier(ack->turn_id, REMOTE_TURN_ID_MAX)) {
        return false;
    }

    if (link->turn_id[0] != '\0' && strcmp(link->turn_id, ack->turn_id) != 0) {
        return false;
    }

    copy_bounded(link->turn_id, sizeof(link->turn_id), ack->turn_id);
    return true;
}

bool remote_link_agent_output(struct remote_link_s* link,
    const struct remote_agent_output_s* output, bool* gap_detected)
{
    bool gap;

    if (gap_detected != NULL) {
        *gap_detected = false;
    }

    if (link == NULL || output == NULL || output->connection_nonce == NULL || output->epoch == NULL || !remote_valid_identifier(output->request_id, REMOTE_REQUEST_ID_MAX) || !remote_valid_identifier(output->turn_id, REMOTE_TURN_ID_MAX) || !remote_valid_identifier(output->connection_nonce, REMOTE_NONCE_MAX) || !remote_valid_identifier(output->epoch, REMOTE_EPOCH_MAX) || !valid_agent_text(output->text) || output->seq == 0 || strcmp(output->connection_nonce, link->nonce) != 0 || strcmp(output->epoch, link->epoch) != 0 || !link->turn_active || strcmp(output->request_id, link->turn_request_id) != 0 || output->seq <= link->last_output_seq) {
        return false;
    }

    if (link->turn_id[0] != '\0' && strcmp(link->turn_id, output->turn_id) != 0) {
        return false;
    }

    if (link->turn_id[0] == '\0') {
        copy_bounded(link->turn_id, sizeof(link->turn_id), output->turn_id);
    }

    /* A QoS 0 gap is noted for display but never changes authority state. */
    gap = link->last_output_seq != 0 && output->seq > link->last_output_seq + 1;
    link->last_output_seq = output->seq;
    link->output_gap = link->output_gap || gap;
    append_transcript(link, output->text);

    if (gap_detected != NULL) {
        *gap_detected = gap;
    }

    return true;
}

bool remote_link_turn_result(struct remote_link_s* link,
    const struct remote_turn_result_s* result)
{
    if (link == NULL || result == NULL || result->connection_nonce == NULL || result->epoch == NULL || !remote_valid_identifier(result->request_id, REMOTE_REQUEST_ID_MAX) || !remote_valid_identifier(result->turn_id, REMOTE_TURN_ID_MAX) || result->status == NULL || !remote_valid_identifier(result->connection_nonce, REMOTE_NONCE_MAX) || !remote_valid_identifier(result->epoch, REMOTE_EPOCH_MAX) || (strcmp(result->status, "completed") != 0 && strcmp(result->status, "failed") != 0) || strcmp(result->connection_nonce, link->nonce) != 0 || strcmp(result->epoch, link->epoch) != 0 || !link->turn_active || strcmp(result->request_id, link->turn_request_id) != 0) {
        return false;
    }

    if (link->turn_id[0] != '\0' && strcmp(link->turn_id, result->turn_id) != 0) {
        return false;
    }

    snprintf(link->summary, sizeof(link->summary), "Agent turn %s", result->status);
    clear_turn(link, false);
    return true;
}

/* ── Outbound preconditions ────────────────────────────────────────── */

bool remote_link_can_submit_prompt(const struct remote_link_s* link,
    const char* text)
{
    /* Refusing while has_prompt is set keeps a local tool call from waiting
     * behind a human permission decision on the remote agent. */
    return link != NULL && link->state != REMOTE_STATE_OFFLINE && link->chat_supported && strcmp(link->chat_epoch, link->epoch) == 0 && !link->turn_active && !link->has_prompt && text != NULL && text[0] != '\0' && strlen(text) <= REMOTE_PROMPT_TEXT_MAX && remote_valid_utf8(text) && has_nonspace(text);
}

const char* remote_link_decision_value(const struct remote_link_s* link,
    enum remote_decision_e decision)
{
    if (link == NULL || !link->has_prompt || !remote_valid_identifier(link->nonce, REMOTE_NONCE_MAX) || !remote_valid_identifier(link->epoch, REMOTE_EPOCH_MAX) || !remote_valid_identifier(link->prompt.id, REMOTE_PROMPT_ID_MAX)) {
        return NULL;
    }

    if (decision == REMOTE_DECISION_ONCE && link->prompt.can_once) {
        return "once";
    }

    if (decision == REMOTE_DECISION_DENY && link->prompt.can_deny) {
        return "deny";
    }

    return NULL;
}

bool remote_link_can_query_usage(const struct remote_link_s* link)
{
    return link != NULL && link->state != REMOTE_STATE_OFFLINE && link->epoch[0] != '\0';
}

bool remote_link_can_select_model(const struct remote_link_s* link,
    const char* model)
{
    /* Refused mid-turn so a reply cannot be produced by two different models.
     * The remembered list is deliberately not consulted: it can be truncated or
     * stale, and the bridge is the only authority on what is selectable. */
    return link != NULL && link->state != REMOTE_STATE_OFFLINE && link->epoch[0] != '\0' && !link->turn_active && remote_valid_identifier(model, REMOTE_MODEL_MAX);
}

/* Index resolution lives with the console command, against the same copy it
 * printed — see the note in remote_link.h. */

/* ── Display helpers ───────────────────────────────────────────────── */

bool remote_link_billing_text(const struct remote_link_s* link, char* out,
    size_t out_size)
{
    int written;
    size_t used;

    if (link == NULL || out == NULL || out_size == 0) {
        return false;
    }

    written = snprintf(out, out_size, "Billing: ");
    if (written < 0 || (size_t)written >= out_size) {
        return false;
    }
    used = (size_t)written;

    /* An absent slot stays empty: never substitute zero, never derive a price
     * from tokens, never show context-window use as billing. */
    if (link->usage.has_charge) {
        written = snprintf(out + used, out_size - used, "%.6g %s",
            link->usage.charge_amount, link->usage.charge_unit);
        if (written < 0 || (size_t)written >= out_size - used) {
            return false;
        }
        used += (size_t)written;
    }

    written = snprintf(out + used, out_size - used, " / ");
    if (written < 0 || (size_t)written >= out_size - used) {
        return false;
    }
    used += (size_t)written;

    if (link->usage.has_tokens) {
        written = snprintf(out + used, out_size - used, "%" PRIu64 " tokens",
            link->usage.total_tokens);
        if (written < 0 || (size_t)written >= out_size - used) {
            return false;
        }
    }

    return true;
}

const char* remote_state_name(enum remote_state_e state)
{
    switch (state) {
    case REMOTE_STATE_OFFLINE:
        return "Offline";
    case REMOTE_STATE_IDLE:
        return "Idle";
    case REMOTE_STATE_BUSY:
        return "Busy";
    case REMOTE_STATE_ATTENTION:
        return "Attention";
    case REMOTE_STATE_RESULT:
        return "Result";
    default:
        return "Unknown";
    }
}
