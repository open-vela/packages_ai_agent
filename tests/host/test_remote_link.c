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

/* Host test for the remote-control protocol core.
 *
 * The link, codec, and session layers have no NuttX or MQTT dependency, so the
 * authority rules can be checked on a development machine. Build and run:
 *
 *   tests/host/run_remote_link_test.sh
 *
 * This is protocol-level coverage only. It says nothing about MQTT transport,
 * board bring-up, or a real bridge.
 */

#include "remote/remote_codec.h"
#include "remote/remote_session.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* ── Capture transport ─────────────────────────────────────────────── */

struct capture_s {
    unsigned int sends;
    bool fail_next;
    char message[REMOTE_MESSAGE_MAX + 1];
};

static bool capture_send(void* context, const char* json, size_t length)
{
    struct capture_s* capture = context;

    assert(length <= REMOTE_MESSAGE_MAX);

    if (capture->fail_next) {
        capture->fail_next = false;
        return false;
    }

    memcpy(capture->message, json, length);
    capture->message[length] = '\0';
    capture->sends++;
    return true;
}

static const struct remote_transport_ops_s g_capture_ops = {
    .send_json = capture_send,
};

/* ── Event capture ─────────────────────────────────────────────────── */

struct events_s {
    unsigned int state;
    unsigned int output;
    unsigned int gaps;
    unsigned int transcripts;
    unsigned int turn_results;
    unsigned int rejected;
    unsigned int billing;
    unsigned int models;
    /* The reply as the handler received it, plus the order it arrived in
     * relative to the turn result: a reply printed after "turn completed" would
     * read backwards. */
    char transcript[REMOTE_TRANSCRIPT_MAX + 1];
    bool transcript_before_result;
};

static void on_event(void* context, enum remote_event_e event, const char* text)
{
    struct events_s* events = context;

    switch (event) {
    case REMOTE_EVENT_STATE:
        events->state++;
        break;
    case REMOTE_EVENT_AGENT_OUTPUT:
        events->output++;
        break;
    case REMOTE_EVENT_OUTPUT_GAP:
        events->gaps++;
        break;
    case REMOTE_EVENT_TURN_TRANSCRIPT:
        events->transcripts++;
        if (text != NULL) {
            snprintf(events->transcript, sizeof(events->transcript), "%s", text);
        }
        if (events->turn_results == 0) {
            events->transcript_before_result = true;
        }
        break;
    case REMOTE_EVENT_TURN_RESULT:
        events->turn_results++;
        break;
    case REMOTE_EVENT_PROMPT_REJECTED:
        events->rejected++;
        break;
    case REMOTE_EVENT_BILLING:
        events->billing++;
        break;
    case REMOTE_EVENT_MODEL:
        events->models++;
        break;
    default:
        break;
    }
}

/* ── Authority, permission latch, and billing ──────────────────────── */

static void test_authority_and_permission(void)
{
    struct remote_session_s session;
    struct capture_s capture = { 0 };
    struct events_s events = { 0 };
    struct remote_transport_s transport = {
        .ops = &g_capture_ops,
        .context = &capture,
    };
    struct remote_prompt_s prompt = {
        .id = "prompt-1",
        .tool = "bash_delete",
        .hint = "rm /tmp/probe",
        .can_once = true,
        .can_deny = true,
    };
    struct remote_snapshot_s snapshot = {
        .connection_nonce = "nonce",
        .epoch = "epoch",
        .seq = 1,
        .running = 0,
        .summary = "approval required",
        .prompt = &prompt,
    };
    struct remote_usage_snapshot_s usage = {
        .connection_nonce = "nonce",
        .epoch = "epoch",
        .seq = 1,
        .has_charge = true,
        .charge_kind = REMOTE_CHARGE_CREDIT,
        .charge_amount = 0.375,
        .charge_unit = "credits",
        .has_tokens = true,
        .input_tokens = 11,
        .output_tokens = 3,
        .total_tokens = 14,
        .has_context = true,
        .context_unit = REMOTE_CONTEXT_TOKEN,
        .context_used = 17,
        .context_limit = 1000,
    };
    struct remote_status_s status;
    char overlong_summary[REMOTE_SUMMARY_MAX + 2];
    bool changed;

    remote_session_init(&session, "nonce");
    remote_session_set_event_handler(&session, on_event, &events);
    remote_session_set_active_transport(&session, &transport);

    /* First snapshot locks the epoch and raises the attention state. */
    assert(remote_session_snapshot(&session, &snapshot, 100, &changed));
    assert(changed);
    remote_session_status(&session, &status);
    assert(status.state == REMOTE_STATE_ATTENTION);
    assert(status.has_prompt);
    assert(events.state == 1);

    /* Usage telemetry is independent of authority and must not disturb it. */
    assert(remote_session_usage_snapshot(&session, &usage, &changed));
    assert(changed);
    assert(events.billing == 1);
    remote_session_status(&session, &status);
    assert(status.state == REMOTE_STATE_ATTENTION);
    assert(status.has_prompt);
    assert(strcmp(status.billing, "Billing: 0.375 credits / 14 tokens") == 0);

    /* A repeated usage sequence is stale and rejected. */
    assert(!remote_session_usage_snapshot(&session, &usage, &changed));

    /* A QoS 0 gap may skip a sequence; a newer one still applies. Either
     * billing slot may be absent and is then rendered empty, never zero. */
    usage.seq = 3;
    usage.has_charge = false;
    usage.has_tokens = true;
    usage.total_tokens = 25;
    assert(remote_session_usage_snapshot(&session, &usage, &changed));
    remote_session_status(&session, &status);
    assert(strcmp(status.billing, "Billing:  / 25 tokens") == 0);

    usage.seq = 2;
    assert(!remote_session_usage_snapshot(&session, &usage, &changed));

    usage.seq = 4;
    usage.has_charge = true;
    usage.charge_amount = 0.5;
    usage.has_tokens = false;
    assert(remote_session_usage_snapshot(&session, &usage, &changed));
    remote_session_status(&session, &status);
    assert(strcmp(status.billing, "Billing: 0.5 credits / ") == 0);

    /* Read-only query is allowed while a permission is pending. */
    assert(remote_session_query_usage(&session));
    assert(capture.sends == 1);
    assert(strstr(capture.message, "\"cmd\":\"usage_query\"") != NULL);
    assert(strstr(capture.message, "\"connection_nonce\":\"nonce\"") != NULL);

    /* One decision per prompt: the second is refused locally. */
    assert(remote_session_decide(&session, REMOTE_DECISION_DENY));
    assert(capture.sends == 2);
    assert(strstr(capture.message, "\"decision\":\"deny\"") != NULL);
    assert(strstr(capture.message, "\"cmd\":\"permission\"") != NULL);
    assert(!remote_session_decide(&session, REMOTE_DECISION_DENY));
    assert(!remote_session_decide(&session, REMOTE_DECISION_ONCE));
    assert(capture.sends == 2);

    /* A repeat of the same prompt keeps the latch closed. */
    snapshot.seq = 2;
    assert(remote_session_snapshot(&session, &snapshot, 101, &changed));
    assert(!changed);
    remote_session_status(&session, &status);
    assert(status.decision_in_flight);

    /* An unrelated tool result is not authority to clear a pending prompt. */
    assert(!remote_session_tool_result(&session, "nonce", "epoch",
        "unrelated-tool", "completed", NULL));
    remote_session_status(&session, &status);
    assert(status.has_prompt);
    assert(status.decision_in_flight);

    /* An oversized summary and a wrong sequence are both rejected. */
    memset(overlong_summary, 'x', sizeof(overlong_summary) - 1);
    overlong_summary[sizeof(overlong_summary) - 1] = '\0';
    snapshot.seq = 3;
    snapshot.summary = overlong_summary;
    assert(!remote_session_snapshot(&session, &snapshot, 102, &changed));

    snapshot.summary = "approval required";
    snapshot.seq = 4; /* expected 3 */
    assert(!remote_session_snapshot(&session, &snapshot, 102, &changed));

    /* Liveness is independent of the socket: no snapshots means Offline. */
    assert(remote_session_timeout(&session, 30102));
    remote_session_status(&session, &status);
    assert(status.state == REMOTE_STATE_OFFLINE);
    assert(!status.has_prompt);
    assert(strcmp(status.billing, "Billing:  / ") == 0);

    /* With no prompt pending, a tool result may report a result state. With no
     * title, the tool-call id is the fallback label. */
    assert(remote_session_tool_result(&session, "nonce", "", "tool-1",
        "completed", NULL));
    remote_session_status(&session, &status);
    assert(status.state == REMOTE_STATE_RESULT);
    assert(strcmp(status.summary, "tool 'tool-1' completed") == 0);
    assert(!remote_session_tool_result(&session, "nonce", "", "tool-1",
        "pending", NULL));

    /* A title replaces the unreadable id, and an empty one falls back to it. */
    assert(remote_session_tool_result(&session, "nonce", "", "tooluse_9xQz",
        "completed", "Delete /tmp/1.txt"));
    remote_session_status(&session, &status);
    assert(strcmp(status.summary, "tool 'Delete /tmp/1.txt' completed") == 0);

    assert(remote_session_tool_result(&session, "nonce", "", "tooluse_9xQz",
        "failed", ""));
    remote_session_status(&session, &status);
    assert(strcmp(status.summary, "tool 'tooluse_9xQz' failed") == 0);

    /* A control byte in a title would split the one-line status into two, so it
     * is flattened to a space rather than passed through. */
    assert(remote_session_tool_result(&session, "nonce", "", "tool-2",
        "completed", "line\nbreak\ttab"));
    remote_session_status(&session, &status);
    assert(strcmp(status.summary, "tool 'line break tab' completed") == 0);

    remote_session_destroy(&session);
    printf("  authority, permission latch, billing: ok\n");
}

/* ── Chat turns ────────────────────────────────────────────────────── */

static void test_chat_turns(void)
{
    struct remote_session_s session;
    struct capture_s capture = { 0 };
    struct events_s events = { 0 };
    struct remote_transport_s transport = {
        .ops = &g_capture_ops,
        .context = &capture,
    };
    struct remote_hello_ack_s hello_ack = {
        .connection_nonce = "chat-nonce",
        .epoch = "chat-epoch",
        .chat_turns = true,
    };
    struct remote_snapshot_s snapshot = {
        .connection_nonce = "chat-nonce",
        .epoch = "chat-epoch",
        .seq = 1,
        .running = 0,
        .summary = "Agent ready",
        .prompt = NULL,
    };
    struct remote_prompt_ack_s prompt_ack = {
        .connection_nonce = "chat-nonce",
        .epoch = "chat-epoch",
        .request_id = "r-chat-nonce-00000001",
        .turn_id = "turn-1",
        .accepted = true,
    };
    struct remote_agent_output_s output = {
        .connection_nonce = "chat-nonce",
        .epoch = "chat-epoch",
        .request_id = "r-chat-nonce-00000001",
        .turn_id = "turn-1",
        .seq = 1,
        .text = "first answer\n",
    };
    struct remote_turn_result_s turn_result = {
        .connection_nonce = "chat-nonce",
        .epoch = "chat-epoch",
        .request_id = "r-chat-nonce-00000001",
        .turn_id = "turn-1",
        .status = "completed",
    };
    struct remote_status_s status;
    char transcript[REMOTE_TRANSCRIPT_MAX + 1];
    bool changed;
    bool gap;

    remote_session_init(&session, "chat-nonce");
    remote_session_set_event_handler(&session, on_event, &events);
    remote_session_set_active_transport(&session, &transport);

    /* No prompt may be sent before the bridge advertises chat turns. */
    assert(!remote_session_submit_prompt(&session, "too early"));

    assert(remote_session_hello_ack(&session, &hello_ack));
    assert(remote_session_snapshot(&session, &snapshot, 1, &changed));

    /* cJSON owns the escaping, so quotes in the prompt cannot break the wire
     * form. */
    assert(remote_session_submit_prompt(&session, "inspect \"this\""));
    assert(capture.sends == 1);
    assert(strstr(capture.message, "\"cmd\":\"prompt_submit\"") != NULL);
    assert(strstr(capture.message, "inspect \\\"this\\\"") != NULL);
    assert(strstr(capture.message, "\"request_id\":\"r-chat-nonce-00000001\"") != NULL);

    /* The turn slot is reserved, so a second submit is refused. */
    assert(!remote_session_submit_prompt(&session, "second"));
    assert(capture.sends == 1);

    assert(remote_session_prompt_ack(&session, &prompt_ack));
    assert(remote_session_agent_output(&session, &output, &gap));
    assert(!gap);
    assert(events.output == 1);
    assert(remote_session_transcript(&session, transcript, sizeof(transcript)));
    assert(strcmp(transcript, "first answer\n") == 0);

    /* A skipped output sequence is noted but does not change authority. */
    output.seq = 3;
    output.text = "third answer";
    assert(remote_session_agent_output(&session, &output, &gap));
    assert(gap);
    assert(events.gaps == 1);

    /* Replays and wrong turn ids are dropped. */
    assert(!remote_session_agent_output(&session, &output, &gap));
    output.seq = 4;
    output.turn_id = "wrong-turn";
    assert(!remote_session_agent_output(&session, &output, &gap));

    assert(remote_session_turn_result(&session, &turn_result));
    assert(events.turn_results == 1);
    remote_session_status(&session, &status);
    assert(!status.turn_active);
    assert(remote_session_transcript(&session, transcript, sizeof(transcript)));
    assert(strstr(transcript, "third answer") != NULL);

    /* The whole reply is handed over once, as one piece, before the turn result.
     * This is what makes a reply readable: the chunks that arrived separately are
     * never presented separately. */
    assert(events.transcripts == 1);
    assert(events.transcript_before_result);
    assert(strcmp(events.transcript, "first answer\nthird answer") == 0);
    assert(events.output == 2);

    /* A turn that produced no text emits no transcript event, so nothing prints
     * an empty reply. Submitting clears the previous transcript, which is why the
     * check has to happen on a fresh turn rather than by inspecting this one. */
    prompt_ack.request_id = "r-chat-nonce-00000002";
    prompt_ack.turn_id = "turn-2";
    turn_result.request_id = "r-chat-nonce-00000002";
    turn_result.turn_id = "turn-2";
    assert(remote_session_submit_prompt(&session, "a turn that only runs tools"));
    assert(remote_session_prompt_ack(&session, &prompt_ack));
    assert(remote_session_turn_result(&session, &turn_result));
    assert(events.transcripts == 1);
    assert(events.turn_results == 2);

    remote_session_destroy(&session);
    printf("  chat turns, output sequencing, escaping: ok\n");
}

/* ── Send failure rolls the turn back ──────────────────────────────── */

static void test_send_failure_releases_turn(void)
{
    struct remote_session_s session;
    struct capture_s capture = { 0 };
    struct remote_transport_s transport = {
        .ops = &g_capture_ops,
        .context = &capture,
    };
    struct remote_hello_ack_s hello_ack = {
        .connection_nonce = "n2",
        .epoch = "e2",
        .chat_turns = true,
    };
    struct remote_snapshot_s snapshot = {
        .connection_nonce = "n2",
        .epoch = "e2",
        .seq = 1,
        .running = 0,
        .summary = "ready",
        .prompt = NULL,
    };
    struct remote_status_s status;
    bool changed;

    remote_session_init(&session, "n2");
    remote_session_set_active_transport(&session, &transport);
    assert(remote_session_hello_ack(&session, &hello_ack));
    assert(remote_session_snapshot(&session, &snapshot, 1, &changed));

    /* A publish that fails must not leave the turn slot reserved, or the
     * device could never send another prompt on this connection. */
    capture.fail_next = true;
    assert(!remote_session_submit_prompt(&session, "will not send"));
    remote_session_status(&session, &status);
    assert(!status.turn_active);

    assert(remote_session_submit_prompt(&session, "retry"));
    remote_session_status(&session, &status);
    assert(status.turn_active);

    remote_session_destroy(&session);
    printf("  send failure releases the turn slot: ok\n");
}

/* ── Inbound wire decoding ─────────────────────────────────────────── */

static void test_codec_receive(void)
{
    struct remote_session_s session;
    struct capture_s capture = { 0 };
    struct events_s events = { 0 };
    struct remote_transport_s transport = {
        .ops = &g_capture_ops,
        .context = &capture,
    };
    struct remote_status_s status;
    char oversized[REMOTE_MESSAGE_MAX + 64];
    const char* snapshot_json
        = "{\"v\":1,\"type\":\"snapshot\",\"connection_nonce\":\"wire-nonce\","
          "\"epoch\":\"wire-epoch\",\"seq\":1,\"running\":0,"
          "\"msg\":\"waiting for you\",\"prompt\":{\"id\":\"p\\\"quoted\","
          "\"tool\":\"bash\",\"hint\":\"rm -rf /tmp/x\",\"canOnce\":true,"
          "\"canDeny\":true}}";

    remote_session_init(&session, "wire-nonce");
    remote_session_set_event_handler(&session, on_event, &events);
    remote_session_set_active_transport(&session, &transport);

    remote_codec_receive(&session, snapshot_json, strlen(snapshot_json), 500);
    remote_session_status(&session, &status);
    assert(status.state == REMOTE_STATE_ATTENTION);
    assert(status.has_prompt);
    assert(strcmp(status.summary, "waiting for you") == 0);
    /* An embedded quote survives decode and is re-escaped on the way out. */
    assert(strcmp(status.prompt.id, "p\"quoted") == 0);
    assert(remote_session_decide(&session, REMOTE_DECISION_ONCE));
    assert(strstr(capture.message, "p\\\"quoted") != NULL);

    /* A message from another session is ignored. */
    {
        const char* foreign
            = "{\"v\":1,\"type\":\"snapshot\",\"connection_nonce\":\"other\","
              "\"epoch\":\"wire-epoch\",\"seq\":2,\"running\":1,"
              "\"msg\":\"not mine\",\"prompt\":null}";

        remote_codec_receive(&session, foreign, strlen(foreign), 600);
        remote_session_status(&session, &status);
        assert(strcmp(status.summary, "waiting for you") == 0);
    }

    /* Malformed, truncated, and trailing-garbage payloads are all dropped. */
    {
        const char* bad[] = {
            "{",
            "not json at all",
            "{\"v\":2,\"type\":\"snapshot\"}",
            "{\"v\":1,\"type\":\"snapshot\"} trailing",
            "[]",
        };
        size_t index;

        for (index = 0; index < sizeof(bad) / sizeof(bad[0]); index++) {
            remote_codec_receive(&session, bad[index], strlen(bad[index]), 700);
        }
        remote_session_status(&session, &status);
        assert(strcmp(status.summary, "waiting for you") == 0);
    }

    /* An escaped NUL could truncate a nonce into a prefix match, so its only
     * legal wire form is refused before parsing. */
    {
        const char* escaped_nul
            = "{\"v\":1,\"type\":\"snapshot\",\"connection_nonce\":"
              "\"wire-nonce\\u0000ignored\",\"epoch\":\"wire-epoch\","
              "\"seq\":2,\"running\":1,\"msg\":\"nul\",\"prompt\":null}";

        remote_codec_receive(&session, escaped_nul, strlen(escaped_nul), 800);
        remote_session_status(&session, &status);
        assert(strcmp(status.summary, "waiting for you") == 0);
    }

    /* Anything beyond the link cap is dropped whole rather than parsed. */
    memset(oversized, 'x', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    remote_codec_receive(&session, oversized, strlen(oversized), 900);
    remote_session_status(&session, &status);
    assert(strcmp(status.summary, "waiting for you") == 0);

    remote_session_destroy(&session);
    printf("  inbound decoding, session binding, hostile payloads: ok\n");
}

/* ── Offline refuses every outbound operation ──────────────────────── */

static void test_offline_refuses_output(void)
{
    struct remote_session_s session;
    struct capture_s capture = { 0 };
    struct remote_transport_s transport = {
        .ops = &g_capture_ops,
        .context = &capture,
    };

    remote_session_init(&session, "off-nonce");
    remote_session_set_active_transport(&session, &transport);

    assert(!remote_session_submit_prompt(&session, "hello"));
    assert(!remote_session_query_usage(&session));
    assert(!remote_session_decide(&session, REMOTE_DECISION_ONCE));

    /* Only the handshake may leave before any snapshot has arrived. */
    assert(remote_session_send_hello(&session));
    assert(capture.sends == 1);
    assert(strstr(capture.message, "\"cmd\":\"hello\"") != NULL);
    assert(strstr(capture.message, "\"connection_nonce\":\"off-nonce\"") != NULL);

    remote_session_destroy(&session);
    printf("  offline refuses outbound except hello: ok\n");
}

/* ── Model reporting and selection ─────────────────────────────────── */

static void test_model_selection(void)
{
    struct remote_session_s session;
    struct capture_s capture = { 0 };
    struct events_s events = { 0 };
    struct remote_transport_s transport = {
        .ops = &g_capture_ops,
        .context = &capture,
    };
    struct remote_hello_ack_s hello_ack = {
        .connection_nonce = "m-nonce",
        .epoch = "m-epoch",
        .chat_turns = true,
    };
    struct remote_snapshot_s snapshot = {
        .connection_nonce = "m-nonce",
        .epoch = "m-epoch",
        .seq = 1,
        .running = 0,
        .summary = "Agent ready",
        .prompt = NULL,
    };
    /* Deliberately static, as on the device: the option table makes this far too
     * large to want on a small stack. */
    static struct remote_model_view_s view;
    struct remote_status_s status;
    char crowded[REMOTE_MESSAGE_MAX];
    const char* early
        = "{\"v\":1,\"type\":\"models\",\"connection_nonce\":\"m-nonce\","
          "\"epoch\":\"m-epoch\",\"current\":\"kiro/a\",\"options\":[\"kiro/a\"]}";
    const char* offered
        = "{\"v\":1,\"type\":\"models\",\"connection_nonce\":\"m-nonce\","
          "\"epoch\":\"m-epoch\",\"current\":\"kiro/sonnet\","
          "\"options\":[\"kiro/sonnet\",\"kiro/haiku\",\"mify/zhipuai/glm\"],"
          "\"truncated\":false}";
    bool changed;
    size_t used;
    int i;

    remote_session_init(&session, "m-nonce");
    remote_session_set_event_handler(&session, on_event, &events);
    remote_session_set_active_transport(&session, &transport);

    /* Nothing is known before the bridge reports, and nothing may be selected
     * while offline. */
    remote_session_model_view(&session, &view);
    assert(view.current[0] == '\0');
    assert(view.count == 0);
    assert(!remote_session_select_model(&session, "kiro/haiku"));

    /* Models arriving before the first snapshot are dropped: the session epoch
     * is not locked yet, so nothing epoch-stamped can be trusted. This is the
     * ordering the bridge has to respect. */
    remote_codec_receive(&session, early, strlen(early), 100);
    remote_session_model_view(&session, &view);
    assert(view.current[0] == '\0');
    assert(events.models == 0);

    assert(remote_session_hello_ack(&session, &hello_ack));
    assert(remote_session_snapshot(&session, &snapshot, 100, &changed));

    remote_codec_receive(&session, offered, strlen(offered), 200);
    remote_session_model_view(&session, &view);
    assert(strcmp(view.current, "kiro/sonnet") == 0);
    assert(view.count == 3);
    assert(!view.truncated);
    assert(strcmp(view.options[2], "mify/zhipuai/glm") == 0);
    assert(events.models == 1);

    /* The model also shows up in the status a console prints. */
    remote_session_status(&session, &status);
    assert(strcmp(status.model, "kiro/sonnet") == 0);

    /* The bridge repeats this message on every handshake. An unchanged model
     * must not announce itself again, or a quiet link turns into a stream. */
    remote_codec_receive(&session, offered, strlen(offered), 300);
    assert(events.models == 1);

    /* Selecting sends a request; it does not decide. The device only learns the
     * model in use from a later `models` message. */
    capture.sends = 0;
    assert(remote_session_select_model(&session, "mify/zhipuai/glm"));
    assert(capture.sends == 1);
    assert(strstr(capture.message, "\"cmd\":\"model_select\"") != NULL);
    assert(strstr(capture.message, "\"model\":\"mify/zhipuai/glm\"") != NULL);
    remote_session_status(&session, &status);
    assert(strcmp(status.model, "kiro/sonnet") == 0);

    /* A turn owns the model that started it, so a switch is refused until the
     * turn ends rather than changing models mid-reply. */
    assert(remote_session_submit_prompt(&session, "work on this"));
    capture.sends = 0;
    assert(!remote_session_select_model(&session, "kiro/haiku"));
    assert(capture.sends == 0);
    remote_session_model_view(&session, &view);
    assert(view.turn_active);

    /* More names than the device holds: the extras are dropped and the list is
     * reported as partial, so the console never implies it showed everything. */
    used = (size_t)snprintf(crowded, sizeof(crowded),
        "{\"v\":1,\"type\":\"models\",\"connection_nonce\":\"m-nonce\","
        "\"epoch\":\"m-epoch\",\"current\":\"kiro/sonnet\",\"options\":[");
    for (i = 0; i < REMOTE_MODEL_OPTIONS_MAX + 8; i++) {
        used += (size_t)snprintf(crowded + used, sizeof(crowded) - used,
            "%s\"kiro/m%d\"", i > 0 ? "," : "", i);
    }
    snprintf(crowded + used, sizeof(crowded) - used, "]}");
    remote_codec_receive(&session, crowded, strlen(crowded), 400);
    remote_session_model_view(&session, &view);
    assert(view.count == REMOTE_MODEL_OPTIONS_MAX);
    assert(view.truncated);

    /* A losing connection forgets the model rather than keeping a stale one: the
     * next bridge may be a different session entirely. */
    remote_session_link_down(&session);
    remote_session_model_view(&session, &view);
    assert(view.current[0] == '\0');
    assert(view.count == 0);
    remote_session_status(&session, &status);
    assert(status.model[0] == '\0');

    remote_session_destroy(&session);
    printf("  model reporting, selection, truncation: ok\n");
}

int main(void)
{
    printf("remote-control protocol core:\n");
    test_authority_and_permission();
    test_chat_turns();
    test_send_failure_releases_turn();
    test_codec_receive();
    test_offline_refuses_output();
    test_model_selection();
    printf("all remote-control protocol tests passed\n");
    return 0;
}
