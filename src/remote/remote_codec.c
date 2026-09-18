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

#include "remote/remote_codec.h"
#include "remote/remote_session.h"

#include "cJSON.h"

#include <float.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Staging buffer for one inbound message. The device runs a single remote
 * session, and the transport delivers messages one at a time from its own
 * thread, so a single buffer is sufficient and keeps this off the caller's
 * stack. */
static char g_inbound[REMOTE_MESSAGE_MAX + 1];

/* ── Field helpers ─────────────────────────────────────────────────── */

static bool valid_string(const cJSON* item)
{
    return cJSON_IsString(item) && item->valuestring != NULL;
}

static const cJSON* field(const cJSON* root, const char* name)
{
    return cJSON_GetObjectItemCaseSensitive(root, name);
}

/* cJSON decodes JSON \u0000 into a C string terminator. Reject its only legal
 * wire representation before parsing so strcmp cannot turn an extended nonce
 * or epoch into a prefix match. */
static bool contains_escaped_nul(const char* payload, size_t length)
{
    size_t index;

    for (index = 0; index < length; index++) {
        if (payload[index] != '\\' || index + 1 >= length) {
            continue;
        }

        if (payload[index + 1] == '\\') {
            index++;
            continue;
        }

        if (payload[index + 1] == 'u' && index + 5 < length) {
            if (payload[index + 2] == '0' && payload[index + 3] == '0' && payload[index + 4] == '0' && payload[index + 5] == '0') {
                return true;
            }
            index += 5;
        } else {
            index++;
        }
    }

    return false;
}

static bool has_chat_turns_capability(const cJSON* capabilities)
{
    const cJSON* entry;

    if (capabilities == NULL || !cJSON_IsArray(capabilities)) {
        return false;
    }

    cJSON_ArrayForEach(entry, capabilities)
    {
        if (!valid_string(entry) || strlen(entry->valuestring) > 32) {
            return false;
        }
        if (strcmp(entry->valuestring, "chat_turns") == 0) {
            return true;
        }
    }

    return false;
}

/* ── Scalar parsers ────────────────────────────────────────────────── */

/* JSON numbers pass through cJSON as doubles. Stay inside JavaScript's
 * exact-integer range: the Node bridge cannot faithfully emit larger integer
 * token counts either. */
static bool parse_uint64(const cJSON* value, uint64_t* out)
{
    const double maximum = 9007199254740991.0;
    uint64_t converted;

    if (!cJSON_IsNumber(value) || value->valuedouble != value->valuedouble || value->valuedouble < 0 || value->valuedouble > maximum) {
        return false;
    }

    converted = (uint64_t)value->valuedouble;
    if (value->valuedouble != (double)converted) {
        return false;
    }

    *out = converted;
    return true;
}

static bool parse_uint32(const cJSON* value, uint32_t* out)
{
    const double maximum = 4294967295.0;
    uint32_t converted;

    if (!cJSON_IsNumber(value) || value->valuedouble != value->valuedouble || value->valuedouble < 0 || value->valuedouble > maximum) {
        return false;
    }

    converted = (uint32_t)value->valuedouble;
    if (value->valuedouble != (double)converted) {
        return false;
    }

    *out = converted;
    return true;
}

static bool parse_nonnegative_number(const cJSON* value, double* out)
{
    if (!cJSON_IsNumber(value) || value->valuedouble < 0 || value->valuedouble != value->valuedouble || value->valuedouble > DBL_MAX) {
        return false;
    }

    *out = value->valuedouble;
    return true;
}

static bool valid_currency(const char* value)
{
    return value != NULL && strlen(value) == 3 && value[0] >= 'A' && value[0] <= 'Z' && value[1] >= 'A' && value[1] <= 'Z' && value[2] >= 'A' && value[2] <= 'Z';
}

/* ── Composite parsers ─────────────────────────────────────────────── */

static bool parse_prompt(const cJSON* value, struct remote_prompt_s* prompt)
{
    const cJSON* id;
    const cJSON* tool;
    const cJSON* hint;
    const cJSON* can_once;
    const cJSON* can_deny;

    if (cJSON_IsNull(value)) {
        return true;
    }

    if (!cJSON_IsObject(value)) {
        return false;
    }

    id = field(value, "id");
    tool = field(value, "tool");
    hint = field(value, "hint");
    can_once = field(value, "canOnce");
    can_deny = field(value, "canDeny");
    if (!valid_string(id) || !valid_string(tool) || !valid_string(hint) || !cJSON_IsBool(can_once) || !cJSON_IsBool(can_deny) || strlen(id->valuestring) > REMOTE_PROMPT_ID_MAX || strlen(tool->valuestring) > REMOTE_TOOL_MAX || strlen(hint->valuestring) > REMOTE_HINT_MAX) {
        return false;
    }

    memset(prompt, 0, sizeof(*prompt));
    strcpy(prompt->id, id->valuestring);
    strcpy(prompt->tool, tool->valuestring);
    strcpy(prompt->hint, hint->valuestring);
    prompt->can_once = cJSON_IsTrue(can_once);
    prompt->can_deny = cJSON_IsTrue(can_deny);
    return true;
}

static bool parse_charge(const cJSON* value,
    struct remote_usage_snapshot_s* snapshot)
{
    const cJSON* kind;
    const cJSON* amount;
    const cJSON* unit;

    if (cJSON_IsNull(value)) {
        snapshot->has_charge = false;
        return true;
    }

    if (!cJSON_IsObject(value)) {
        return false;
    }

    kind = field(value, "kind");
    amount = field(value, "amount");
    if (!valid_string(kind) || !parse_nonnegative_number(amount, &snapshot->charge_amount)) {
        return false;
    }

    if (strcmp(kind->valuestring, "credit") == 0) {
        snapshot->charge_kind = REMOTE_CHARGE_CREDIT;
        unit = field(value, "unit");
    } else if (strcmp(kind->valuestring, "currency") == 0) {
        snapshot->charge_kind = REMOTE_CHARGE_CURRENCY;
        unit = field(value, "currency");
    } else {
        return false;
    }

    if (!valid_string(unit) || unit->valuestring[0] == '\0' || strlen(unit->valuestring) > REMOTE_BILLING_UNIT_MAX || (snapshot->charge_kind == REMOTE_CHARGE_CREDIT && strcmp(unit->valuestring, "credits") != 0) || (snapshot->charge_kind == REMOTE_CHARGE_CURRENCY && !valid_currency(unit->valuestring))) {
        return false;
    }

    snapshot->has_charge = true;
    snapshot->charge_unit = unit->valuestring;
    return true;
}

static bool parse_tokens(const cJSON* value,
    struct remote_usage_snapshot_s* snapshot)
{
    if (cJSON_IsNull(value)) {
        snapshot->has_tokens = false;
        return true;
    }

    if (!cJSON_IsObject(value)) {
        return false;
    }

    if (!parse_uint64(field(value, "input"), &snapshot->input_tokens) || !parse_uint64(field(value, "output"), &snapshot->output_tokens) || !parse_uint64(field(value, "total"), &snapshot->total_tokens)) {
        return false;
    }

    snapshot->has_tokens = true;
    return true;
}

static bool parse_context(const cJSON* value,
    struct remote_usage_snapshot_s* snapshot)
{
    const cJSON* unit;

    if (cJSON_IsNull(value)) {
        snapshot->has_context = false;
        return true;
    }

    if (!cJSON_IsObject(value)) {
        return false;
    }

    unit = field(value, "unit");
    if (!valid_string(unit) || !parse_nonnegative_number(field(value, "used"), &snapshot->context_used) || !parse_nonnegative_number(field(value, "limit"), &snapshot->context_limit) || snapshot->context_limit <= 0) {
        return false;
    }

    if (strcmp(unit->valuestring, "token") == 0) {
        snapshot->context_unit = REMOTE_CONTEXT_TOKEN;
    } else if (strcmp(unit->valuestring, "percent") == 0) {
        snapshot->context_unit = REMOTE_CONTEXT_PERCENT;
    } else {
        return false;
    }

    snapshot->has_context = true;
    return true;
}

/* ── Message handlers ──────────────────────────────────────────────── */

static void receive_snapshot(struct remote_session_s* session,
    const cJSON* root, uint32_t now_ms)
{
    const cJSON* nonce = field(root, "connection_nonce");
    const cJSON* epoch = field(root, "epoch");
    const cJSON* seq = field(root, "seq");
    const cJSON* running = field(root, "running");
    const cJSON* message = field(root, "msg");
    const cJSON* prompt = field(root, "prompt");
    struct remote_snapshot_s snapshot;
    struct remote_prompt_s prompt_value;
    uint32_t sequence;
    uint32_t running_value;
    bool display_changed;

    if (!valid_string(nonce) || !valid_string(epoch) || strlen(nonce->valuestring) > REMOTE_NONCE_MAX || strlen(epoch->valuestring) > REMOTE_EPOCH_MAX || !parse_uint32(seq, &sequence) || sequence == 0 || !parse_uint32(running, &running_value) || running_value > UINT8_MAX || !valid_string(message) || strlen(message->valuestring) > REMOTE_SUMMARY_MAX || prompt == NULL || !parse_prompt(prompt, &prompt_value)) {
        return;
    }

    snapshot.connection_nonce = nonce->valuestring;
    snapshot.epoch = epoch->valuestring;
    snapshot.seq = sequence;
    snapshot.running = (uint8_t)running_value;
    snapshot.summary = message->valuestring;
    snapshot.prompt = cJSON_IsNull(prompt) ? NULL : &prompt_value;
    remote_session_snapshot(session, &snapshot, now_ms, &display_changed);
}

static void receive_usage_snapshot(struct remote_session_s* session,
    const cJSON* root)
{
    const cJSON* nonce = field(root, "connection_nonce");
    const cJSON* epoch = field(root, "epoch");
    const cJSON* sequence = field(root, "usage_seq");
    const cJSON* usage = field(root, "usage");
    const cJSON* scope;
    const cJSON* billing;
    const cJSON* charge;
    const cJSON* tokens;
    const cJSON* context;
    struct remote_usage_snapshot_s snapshot = { 0 };
    uint64_t sequence_value;
    bool display_changed;

    if (!valid_string(nonce) || !valid_string(epoch) || !parse_uint64(sequence, &sequence_value) || sequence_value == 0 || !cJSON_IsObject(usage)) {
        return;
    }

    scope = field(usage, "scope");
    billing = field(usage, "billing");
    context = field(usage, "context");

    /* Only bridge-session-local telemetry is accepted. This is never an
     * account balance or a remaining quota. */
    if (!valid_string(scope) || strcmp(scope->valuestring, "bridge_session") != 0 || !cJSON_IsObject(billing) || context == NULL) {
        return;
    }

    charge = field(billing, "charge");
    tokens = field(billing, "tokens");
    if (charge == NULL || tokens == NULL || !parse_charge(charge, &snapshot) || !parse_tokens(tokens, &snapshot) || !parse_context(context, &snapshot)) {
        return;
    }

    snapshot.connection_nonce = nonce->valuestring;
    snapshot.epoch = epoch->valuestring;
    snapshot.seq = sequence_value;
    remote_session_usage_snapshot(session, &snapshot, &display_changed);
}

static void receive_tool_result(struct remote_session_s* session,
    const cJSON* root)
{
    const cJSON* nonce = field(root, "connection_nonce");
    const cJSON* epoch = field(root, "epoch");
    const cJSON* tool_call_id = field(root, "toolCallId");
    const cJSON* status = field(root, "status");
    const cJSON* title = field(root, "title");
    const char* title_text = NULL;

    if (!valid_string(nonce) || !valid_string(epoch) || !valid_string(tool_call_id) || !valid_string(status) || strlen(nonce->valuestring) > REMOTE_NONCE_MAX || strlen(epoch->valuestring) > REMOTE_EPOCH_MAX || strlen(tool_call_id->valuestring) > REMOTE_PROMPT_ID_MAX) {
        return;
    }

    /* The title is optional and cosmetic, so a malformed one is dropped rather
     * than rejecting the whole result: the link then falls back to the tool-call
     * id and the state machine still advances. An older bridge sends no title
     * at all. */
    if (title != NULL && valid_string(title) && strlen(title->valuestring) <= REMOTE_SUMMARY_MAX && remote_valid_utf8(title->valuestring)) {
        title_text = title->valuestring;
    }

    /* The session checks the nonce/epoch binding under its own lock. */
    remote_session_tool_result(session, nonce->valuestring, epoch->valuestring,
        tool_call_id->valuestring, status->valuestring, title_text);
}

static void receive_models(struct remote_session_s* session, const cJSON* root)
{
    const cJSON* nonce = field(root, "connection_nonce");
    const cJSON* epoch = field(root, "epoch");
    const cJSON* current = field(root, "current");
    const cJSON* options = field(root, "options");
    const cJSON* truncated = field(root, "truncated");
    const cJSON* entry;
    /* Pointers into the parsed cJSON tree, valid until the caller deletes it.
     * remote_link_models copies what it keeps, so nothing outlives this call. */
    const char* names[REMOTE_MODEL_OPTIONS_MAX];
    struct remote_models_s models;
    size_t count = 0;
    bool overflow = false;

    if (!valid_string(nonce) || !valid_string(epoch) || !valid_string(current) || strlen(nonce->valuestring) > REMOTE_NONCE_MAX || strlen(epoch->valuestring) > REMOTE_EPOCH_MAX || (options != NULL && !cJSON_IsArray(options)) || (truncated != NULL && !cJSON_IsBool(truncated))) {
        return;
    }

    if (options != NULL) {
        cJSON_ArrayForEach(entry, options)
        {
            if (count >= REMOTE_MODEL_OPTIONS_MAX) {
                /* More on offer than the device holds. Recorded so the console
                 * can say the list is partial instead of implying it is all. */
                overflow = true;
                break;
            }

            if (!valid_string(entry)) {
                overflow = true;
                continue;
            }

            names[count] = entry->valuestring;
            count++;
        }
    }

    models.connection_nonce = nonce->valuestring;
    models.epoch = epoch->valuestring;
    models.current = current->valuestring;
    models.options = names;
    models.count = count;
    models.truncated = overflow || cJSON_IsTrue(truncated);
    remote_session_models(session, &models);
}

static void receive_hello_ack(struct remote_session_s* session,
    const cJSON* root)
{
    const cJSON* nonce = field(root, "connection_nonce");
    const cJSON* epoch = field(root, "epoch");
    const cJSON* capabilities = field(root, "capabilities");
    struct remote_hello_ack_s ack;

    if (!valid_string(nonce) || !valid_string(epoch) || strlen(nonce->valuestring) > REMOTE_NONCE_MAX || strlen(epoch->valuestring) > REMOTE_EPOCH_MAX || (capabilities != NULL && !cJSON_IsArray(capabilities))) {
        return;
    }

    ack.connection_nonce = nonce->valuestring;
    ack.epoch = epoch->valuestring;
    ack.chat_turns = has_chat_turns_capability(capabilities);
    remote_session_hello_ack(session, &ack);
}

static void receive_prompt_ack(struct remote_session_s* session,
    const cJSON* root)
{
    const cJSON* nonce = field(root, "connection_nonce");
    const cJSON* epoch = field(root, "epoch");
    const cJSON* request_id = field(root, "request_id");
    const cJSON* turn_id = field(root, "turn_id");
    const cJSON* accepted = field(root, "accepted");
    const cJSON* error = field(root, "error");
    struct remote_prompt_ack_s ack;

    if (!valid_string(nonce) || !valid_string(epoch) || !valid_string(request_id) || !cJSON_IsBool(accepted) || strlen(request_id->valuestring) > REMOTE_REQUEST_ID_MAX || strlen(nonce->valuestring) > REMOTE_NONCE_MAX || strlen(epoch->valuestring) > REMOTE_EPOCH_MAX || (cJSON_IsTrue(accepted) && (!valid_string(turn_id) || strlen(turn_id->valuestring) > REMOTE_TURN_ID_MAX)) || (!cJSON_IsTrue(accepted) && error != NULL && (!valid_string(error) || strlen(error->valuestring) > 32))) {
        return;
    }

    ack.connection_nonce = nonce->valuestring;
    ack.epoch = epoch->valuestring;
    ack.request_id = request_id->valuestring;
    ack.turn_id = cJSON_IsTrue(accepted) ? turn_id->valuestring : NULL;
    ack.accepted = cJSON_IsTrue(accepted);
    remote_session_prompt_ack(session, &ack);
}

static void receive_agent_output(struct remote_session_s* session,
    const cJSON* root)
{
    const cJSON* nonce = field(root, "connection_nonce");
    const cJSON* epoch = field(root, "epoch");
    const cJSON* request_id = field(root, "request_id");
    const cJSON* turn_id = field(root, "turn_id");
    const cJSON* seq = field(root, "output_seq");
    const cJSON* text = field(root, "text");
    struct remote_agent_output_s output;
    uint32_t sequence;
    size_t text_length;
    bool gap;

    if (!valid_string(nonce) || !valid_string(epoch) || !valid_string(request_id) || !valid_string(turn_id) || !valid_string(text) || !parse_uint32(seq, &sequence) || sequence == 0 || strlen(request_id->valuestring) > REMOTE_REQUEST_ID_MAX || strlen(nonce->valuestring) > REMOTE_NONCE_MAX || strlen(epoch->valuestring) > REMOTE_EPOCH_MAX || strlen(turn_id->valuestring) > REMOTE_TURN_ID_MAX) {
        return;
    }

    text_length = strlen(text->valuestring);
    if (text_length == 0 || text_length > REMOTE_OUTPUT_TEXT_MAX) {
        return;
    }

    output.connection_nonce = nonce->valuestring;
    output.epoch = epoch->valuestring;
    output.request_id = request_id->valuestring;
    output.turn_id = turn_id->valuestring;
    output.seq = sequence;
    output.text = text->valuestring;
    remote_session_agent_output(session, &output, &gap);
}

static void receive_turn_result(struct remote_session_s* session,
    const cJSON* root)
{
    const cJSON* nonce = field(root, "connection_nonce");
    const cJSON* epoch = field(root, "epoch");
    const cJSON* request_id = field(root, "request_id");
    const cJSON* turn_id = field(root, "turn_id");
    const cJSON* status = field(root, "status");
    struct remote_turn_result_s result;

    if (!valid_string(nonce) || !valid_string(epoch) || !valid_string(request_id) || !valid_string(turn_id) || !valid_string(status) || strlen(request_id->valuestring) > REMOTE_REQUEST_ID_MAX || strlen(nonce->valuestring) > REMOTE_NONCE_MAX || strlen(epoch->valuestring) > REMOTE_EPOCH_MAX || strlen(turn_id->valuestring) > REMOTE_TURN_ID_MAX) {
        return;
    }

    result.connection_nonce = nonce->valuestring;
    result.epoch = epoch->valuestring;
    result.request_id = request_id->valuestring;
    result.turn_id = turn_id->valuestring;
    result.status = status->valuestring;
    remote_session_turn_result(session, &result);
}

/* ── Inbound entry point ───────────────────────────────────────────── */

void remote_codec_receive(struct remote_session_s* session,
    const char* payload, size_t length, uint32_t now_ms)
{
    cJSON* root;
    const char* end = NULL;
    const cJSON* version;
    const cJSON* type;

    /* Oversized messages are dropped whole rather than parsed partially: the
     * link contract caps every message, so anything larger is not ours. */
    if (session == NULL || payload == NULL || length == 0 || length > REMOTE_MESSAGE_MAX) {
        return;
    }

    if (contains_escaped_nul(payload, length)) {
        return;
    }

    memcpy(g_inbound, payload, length);
    g_inbound[length] = '\0';

    /* Require the parse to consume exactly the payload: trailing bytes after a
     * valid object would otherwise pass unnoticed. */
    root = cJSON_ParseWithLengthOpts(g_inbound, length + 1, &end, 1);
    if (root == NULL || end != g_inbound + length || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }

    version = field(root, "v");
    type = field(root, "type");
    if (!cJSON_IsNumber(version) || version->valuedouble != 1 || !valid_string(type)) {
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type->valuestring, "hello_ack") == 0) {
        receive_hello_ack(session, root);
    } else if (strcmp(type->valuestring, "snapshot") == 0) {
        receive_snapshot(session, root, now_ms);
    } else if (strcmp(type->valuestring, "usage_snapshot") == 0) {
        receive_usage_snapshot(session, root);
    } else if (strcmp(type->valuestring, "tool_result") == 0) {
        receive_tool_result(session, root);
    } else if (strcmp(type->valuestring, "prompt_ack") == 0) {
        receive_prompt_ack(session, root);
    } else if (strcmp(type->valuestring, "agent_output") == 0) {
        receive_agent_output(session, root);
    } else if (strcmp(type->valuestring, "turn_result") == 0) {
        receive_turn_result(session, root);
    } else if (strcmp(type->valuestring, "models") == 0) {
        receive_models(session, root);
    }

    cJSON_Delete(root);
}

/* ── Outbound builders ─────────────────────────────────────────────── */

/* Serializes and copies out, enforcing the link's message cap. cJSON owns the
 * escaping, so no command may inject raw text into the wire form. */
static bool finish(cJSON* root, char* out, size_t out_size)
{
    char* json;
    size_t length;
    bool ok = false;

    if (root == NULL) {
        return false;
    }

    json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (json == NULL) {
        return false;
    }

    length = strlen(json);
    if (length <= REMOTE_MESSAGE_MAX && length < out_size) {
        memcpy(out, json, length + 1);
        ok = true;
    }

    free(json);
    return ok;
}

static cJSON* begin_command(const char* command)
{
    cJSON* root = cJSON_CreateObject();

    if (root == NULL) {
        return NULL;
    }

    if (cJSON_AddNumberToObject(root, "v", 1) == NULL || cJSON_AddStringToObject(root, "cmd", command) == NULL) {
        cJSON_Delete(root);
        return NULL;
    }

    return root;
}

bool remote_codec_build_hello(const struct remote_link_s* link, char* out,
    size_t out_size)
{
    cJSON* root;

    if (link == NULL || out == NULL || out_size == 0 || !remote_valid_identifier(link->nonce, REMOTE_NONCE_MAX)) {
        return false;
    }

    root = begin_command("hello");
    if (root == NULL || cJSON_AddStringToObject(root, "connection_nonce", link->nonce) == NULL) {
        cJSON_Delete(root);
        return false;
    }

    return finish(root, out, out_size);
}

bool remote_codec_build_decision(const struct remote_link_s* link,
    enum remote_decision_e decision, char* out, size_t out_size)
{
    const char* value = remote_link_decision_value(link, decision);
    cJSON* root;

    if (value == NULL || out == NULL || out_size == 0) {
        return false;
    }

    root = begin_command("permission");
    if (root == NULL || cJSON_AddStringToObject(root, "connection_nonce", link->nonce) == NULL || cJSON_AddStringToObject(root, "epoch", link->epoch) == NULL || cJSON_AddStringToObject(root, "id", link->prompt.id) == NULL || cJSON_AddStringToObject(root, "decision", value) == NULL) {
        cJSON_Delete(root);
        return false;
    }

    return finish(root, out, out_size);
}

bool remote_codec_build_usage_query(const struct remote_link_s* link,
    char* out, size_t out_size)
{
    cJSON* root;

    if (!remote_link_can_query_usage(link) || out == NULL || out_size == 0) {
        return false;
    }

    root = begin_command("usage_query");
    if (root == NULL || cJSON_AddStringToObject(root, "connection_nonce", link->nonce) == NULL || cJSON_AddStringToObject(root, "epoch", link->epoch) == NULL) {
        cJSON_Delete(root);
        return false;
    }

    return finish(root, out, out_size);
}

bool remote_codec_build_prompt_submit(const struct remote_link_s* link,
    const char* request_id, const char* text, char* out, size_t out_size)
{
    cJSON* root;

    if (!remote_link_can_submit_prompt(link, text) || out == NULL || out_size == 0 || !remote_valid_identifier(request_id, REMOTE_REQUEST_ID_MAX)) {
        return false;
    }

    root = begin_command("prompt_submit");
    if (root == NULL || cJSON_AddStringToObject(root, "connection_nonce", link->nonce) == NULL || cJSON_AddStringToObject(root, "epoch", link->epoch) == NULL || cJSON_AddStringToObject(root, "request_id", request_id) == NULL || cJSON_AddStringToObject(root, "text", text) == NULL) {
        cJSON_Delete(root);
        return false;
    }

    return finish(root, out, out_size);
}

bool remote_codec_build_model_select(const struct remote_link_s* link,
    const char* model, char* out, size_t out_size)
{
    cJSON* root;

    if (!remote_link_can_select_model(link, model) || out == NULL || out_size == 0) {
        return false;
    }

    root = begin_command("model_select");
    if (root == NULL || cJSON_AddStringToObject(root, "connection_nonce", link->nonce) == NULL || cJSON_AddStringToObject(root, "epoch", link->epoch) == NULL || cJSON_AddStringToObject(root, "model", model) == NULL) {
        cJSON_Delete(root);
        return false;
    }

    return finish(root, out, out_size);
}
