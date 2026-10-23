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

#include "channels/remote_ctrl_channel.h"

#ifdef CONFIG_AI_AGENT_REMOTE_CTRL

#include "agent_config.h"
#include "core/message_bus.h"
#include "infra/config_store.h"
#include "remote/remote_link_mqtt.h"
#include "voice/voice_channel.h"

#if defined(CONFIG_AI_AGENT_LVGL_UI) || defined(CONFIG_AI_AGENT_UI_RPMSG)
#include "ui/lvgl_toast.h"
#include "ui/lvgl_ui_channel.h"
#endif

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char* TAG = "remote_ctrl";

/* ── State ─────────────────────────────────────────────────────────── */

static struct remote_session_s g_session;

static char g_broker_host[REMOTE_MQTT_HOST_MAX + 1];
static char g_broker_port[REMOTE_MQTT_PORT_MAX + 1];
static char g_username[REMOTE_MQTT_USER_MAX + 1];
static char g_password[REMOTE_MQTT_PASSWORD_MAX + 1];
static char g_device_id[REMOTE_MQTT_DEVICE_ID_MAX + 1];
static char g_topic_prefix[REMOTE_MQTT_TOPIC_PREFIX_MAX + 1];

static bool g_configured;
static bool g_started;

/* Turn completion, separate from the session's own lock.
 *
 * Lock order is one-way: the event handler runs with the session lock held and
 * then takes g_turn_lock, so no path may hold g_turn_lock while calling into
 * the session. remote_ctrl_channel_prompt() therefore arms the wait, submits
 * with no lock held, and only then blocks. */
static pthread_mutex_t g_turn_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_turn_cond = PTHREAD_COND_INITIALIZER;
static bool g_turn_pending;
static bool g_turn_finished;

/* Who started the in-flight turn. `rask` (submit, async) wants the remote
 * reply mirrored onto the chat surfaces (console card + LVGL + TTS, the same
 * treatment an `ask` reply gets) because nobody is holding its hand. The LLM
 * tool path (prompt, sync) returns the reply as a tool result and the local
 * LLM presents its own answer through that path already — mirroring would
 * show everything twice. Set by each submit entry point before the turn
 * starts, read once when the transcript lands. */
static bool g_mirror_turn;

/* Debug-only simulated approval (see remote_ctrl_channel_debug_pending()).
 * When set, pending() reports an approval and the next decide() answers it
 * locally instead of reaching the session, which has no real prompt. */
static bool g_debug_pending;

/* ── Event handler ─────────────────────────────────────────────────── */

static void notify_observer_state(const char *text);
static void notify_observer_reply(const char *text);

/* Invoked by the session with its lock held: keep this cheap and never call
 * back into remote_session_*. */
static void on_session_event(void* context, enum remote_event_e event,
    const char* text)
{
    (void)context;

    switch (event) {
    case REMOTE_EVENT_STATE:
        /* A status line and nothing else: what the agent is doing, never what it
         * said. The bridge holds to the same rule on its side, so this reports
         * "Agent working", a tool name, or "Agent replying" — the reply itself
         * arrives as REMOTE_EVENT_TURN_TRANSCRIPT once the turn ends.
         *
         * The prefix is what makes the line readable. A bare tool name or a
         * bare tool argument says nothing about which of those it is, and the
         * approval line is the one a person must react to right now -- it
         * names the key. */
        {
            const char *tag = "[状态]";
            char body[160];

            if (text != NULL && strncmp(text, "approval required: ", 19) == 0) {
                tag = "[需要审批]";
                snprintf(body, sizeof(body),
                         "%s   ->  按 S2 通过 / S3 取消", text + 19);
            } else if (text != NULL && strstr(text, "completed") != NULL) {
                tag = "[工具]";
                snprintf(body, sizeof(body), "%s", text);
            } else if (text != NULL &&
                       (strcmp(text, "Agent working") == 0 ||
                        strcmp(text, "Agent replying") == 0)) {
                tag = "[任务]";
                snprintf(body, sizeof(body), "%s",
                         strcmp(text, "Agent working") == 0 ? "进行中" : "回复中");
            } else if (text != NULL && strncmp(text, "Agent turn", 10) == 0) {
                tag = "[任务]";
                snprintf(body, sizeof(body), "%s", text + 11);
            } else {
                snprintf(body, sizeof(body), "%s", text != NULL ? text : "");
            }

            syslog(LOG_INFO, "[%s] %s %s\n", TAG, tag, body);
            notify_observer_state(text);
        }
#if defined(CONFIG_AI_AGENT_LVGL_UI) || defined(CONFIG_AI_AGENT_UI_RPMSG)
        /* Reflect the authority latch onto the approval page: show it the
         * moment a permission request is pending, dismiss it once the prompt
         * clears (decided locally, timed out, or resolved from the dashboard).
         * Reading link state directly is safe — this handler runs with the
         * session lock held and must not re-enter the session, so we never call
         * remote_ctrl_channel_status() here (it would re-lock and deadlock). */
        if (g_session.link.has_prompt) {
            lvgl_ui_channel_set_approval(g_session.link.prompt.tool,
                                         g_session.link.prompt.hint);
        } else {
            lvgl_ui_channel_clear_approval();
        }
#endif
        break;

    case REMOTE_EVENT_AGENT_OUTPUT:
        /* Deliberately silent. A chunk lands every few tens of milliseconds and
         * splits mid-sentence, so logging one line per chunk filled the console
         * without saying anything the 'Agent replying' status line above has not
         * already said. The link accumulates them; the reply prints once, below. */
        break;

    case REMOTE_EVENT_TURN_TRANSCRIPT:
        /* One call for the whole reply, not one per line. nx_vsyslog stamps its
         * prefix once per call, so printing line by line turned a reply into a
         * column of timestamped log records; passing the text in one go lets its
         * own newlines through and it reads as the block it is.
         *
         * No trailing newline here: nx_vsyslog appends one when the text does not
         * already end in it, so adding one would leave a blank line behind every
         * reply. Length is not a concern — CONFIG_SYSLOG_BUFFER is off on this
         * board, so the raw stream writes straight through with no fixed buffer
         * to truncate against. */
        /* agent_main.c already prints the reply to the console.  Logging
         * the whole body here printed it a second time on a board where
         * syslog shares that console.
         */

        syslog(LOG_INFO, "[%s] reply %u bytes\n", TAG,
            text != NULL ? (unsigned)strlen(text) : 0u);
        notify_observer_reply(text);
        /* rask-initiated turns also ride the cli outbound branch so the reply
         * lands where an `ask` reply lands: console card, LVGL chat view and
         * the voice pipeline behind it. Cheap by design — one heap copy and a
         * ring-buffer push; no remote_session_* reentry. `rreply` still reads
         * the session transcript, so nothing is lost if the push fails. */
        if (g_mirror_turn && text != NULL && text[0] != '\0') {
            agent_msg_t msg;
            memset(&msg, 0, sizeof(msg));
            strncpy(msg.channel, "cli", sizeof(msg.channel) - 1);
            msg.content = strdup(text);
            if (msg.content != NULL) {
                if (message_bus_push_outbound(&msg) != OK)
                    free(msg.content);
            }
            g_mirror_turn = false;
        }
#if defined(CONFIG_AI_AGENT_LVGL_UI) || defined(CONFIG_AI_AGENT_UI_RPMSG)
        /* A remote coding turn runs for minutes and the user walked away —
         * the approval toast (above) already owns the "needs you" case; this
         * one says the wait is over. Voice is already covered by the mirror
         * branch, so the toast only needs the first line of the reply. */
        if (text != NULL && text[0] != '\0') {
            char body[112];
            snprintf(body, sizeof(body), "任务完成: %s", text);
            char* nl = strchr(body, '\n');
            if (nl != NULL)
                *nl = '\0';
            lvgl_toast_show_async("远程编码", body, 0x4caf50, 4000);
        }
#endif
        break;

    case REMOTE_EVENT_OUTPUT_GAP:
        syslog(LOG_WARNING, "[%s] agent output may be incomplete\n", TAG);
        break;

    case REMOTE_EVENT_MODEL:
        /* Announced because it can change from the PC side too, and which model
         * answered a prompt is worth knowing without polling 'rstat'. */
        syslog(LOG_INFO, "[%s] model: %s\n", TAG, text != NULL ? text : "");
        break;

    case REMOTE_EVENT_PROMPT_REJECTED:
        pthread_mutex_lock(&g_turn_lock);
        g_turn_finished = true;
        pthread_cond_broadcast(&g_turn_cond);
        pthread_mutex_unlock(&g_turn_lock);
        syslog(LOG_WARNING, "[%s] prompt rejected by bridge\n", TAG);
        break;

    case REMOTE_EVENT_TURN_RESULT:
        pthread_mutex_lock(&g_turn_lock);
        g_turn_finished = true;
        pthread_cond_broadcast(&g_turn_cond);
        pthread_mutex_unlock(&g_turn_lock);
        syslog(LOG_INFO, "[%s] %s\n", TAG, text != NULL ? text : "turn ended");
        break;

    case REMOTE_EVENT_BILLING:
        syslog(LOG_INFO, "[%s] %s\n", TAG, text != NULL ? text : "");
        break;

    case REMOTE_EVENT_SESSIONS:
        /* One write for the whole block (header + rows + footer included), so
         * it lands exactly like `rstat`'s box. The cached copy is never
         * printed by the command — this is the single print site. */
        if (text != NULL && text[0] != '\0') {
            printf("%s", text);
        }
        break;

    default:
        break;
    }
}

/* ── Configuration ─────────────────────────────────────────────────── */

/* Splits "host" or "host:port"; the port defaults to the MQTT standard. */
static void parse_broker(const char* broker)
{
    const char* colon = strrchr(broker, ':');
    size_t host_length;

    if (colon != NULL && colon != broker) {
        host_length = (size_t)(colon - broker);
        if (host_length > REMOTE_MQTT_HOST_MAX) {
            host_length = REMOTE_MQTT_HOST_MAX;
        }
        memcpy(g_broker_host, broker, host_length);
        g_broker_host[host_length] = '\0';
        snprintf(g_broker_port, sizeof(g_broker_port), "%.*s",
            (int)sizeof(g_broker_port) - 1, colon + 1);
    } else {
        snprintf(g_broker_host, sizeof(g_broker_host), "%s", broker);
        snprintf(g_broker_port, sizeof(g_broker_port), "%d",
            AGENT_MQTT_DEFAULT_PORT);
    }
}

/* ── Transport thread ──────────────────────────────────────────────── */

static void* transport_task(void* arg)
{
    struct remote_mqtt_config_s config;

    (void)arg;

    memset(&config, 0, sizeof(config));
    config.host = g_broker_host;
    config.port = g_broker_port;
    config.username = g_username;
    config.password = g_password;
    config.device_id = g_device_id;
    config.topic_prefix = g_topic_prefix[0] != '\0' ? g_topic_prefix : NULL;

    /* Blocks until remote_ctrl_channel_stop(). */
    remote_link_mqtt_run(&config, &g_session);
    return NULL;
}

/* ── Lifecycle ─────────────────────────────────────────────────────── */

int remote_ctrl_channel_init(void)
{
    char broker[REMOTE_MQTT_HOST_MAX + REMOTE_MQTT_PORT_MAX + 2];

    memset(broker, 0, sizeof(broker));
    memset(g_broker_host, 0, sizeof(g_broker_host));
    memset(g_broker_port, 0, sizeof(g_broker_port));
    memset(g_username, 0, sizeof(g_username));
    memset(g_password, 0, sizeof(g_password));
    memset(g_device_id, 0, sizeof(g_device_id));
    memset(g_topic_prefix, 0, sizeof(g_topic_prefix));
    g_configured = false;
    g_started = false;

    /* Credentials come from the config store, never from argv: a password in a
     * command line is visible in the process list. */
    claw_config_get(AGENT_CFG_KEY_REMOTE_BROKER, broker, sizeof(broker));
    claw_config_get(AGENT_CFG_KEY_REMOTE_DEVICE_ID, g_device_id,
        sizeof(g_device_id));
    claw_config_get(AGENT_CFG_KEY_REMOTE_USERNAME, g_username,
        sizeof(g_username));
    claw_config_get(AGENT_CFG_KEY_REMOTE_PASSWORD, g_password,
        sizeof(g_password));
    claw_config_get(AGENT_CFG_KEY_REMOTE_TOPIC_PREFIX, g_topic_prefix,
        sizeof(g_topic_prefix));

    /* The session is usable before any link exists: it simply reports Offline
     * and refuses every outbound operation. */
    remote_session_init(&g_session, "");
    remote_session_set_event_handler(&g_session, on_session_event, NULL);

    if (broker[0] == '\0') {
        syslog(LOG_INFO, "[%s] no remote_broker configured, channel idle\n",
            TAG);
        return OK;
    }

    if (g_device_id[0] == '\0') {
        snprintf(g_device_id, sizeof(g_device_id), "%s", AGENT_NODE_ID);
    }

    parse_broker(broker);
    g_configured = true;

    syslog(LOG_INFO, "[%s] initialized (broker=%s:%s device=%s)\n", TAG,
        g_broker_host, g_broker_port, g_device_id);
    return OK;
}

/* Voice is a control input to this channel, not a chat message: an
 * utterance from a PTT button is an instruction for the remote agent, the
 * same thing `rask` sends.  Deliberately remote_ctrl_channel_submit() and
 * not decide() -- the voice path never produces a permission decision, it
 * produces a prompt that later demands one, so the "a permission decision
 * is a human typing ronce/rdeny" rule stays intact and no keyword table or
 * intent classifier is needed.
 */

static void voice_sink_to_remote(const char* text, void* ctx)
{
    char reply[256];

    (void)ctx;

    /* Non-blocking for the same reason `rask` is: a turn that stops for a
     * remote permission request has to leave the console and the button
     * adapter free to answer it.
     */

    if (remote_ctrl_channel_submit(text, reply, sizeof(reply)) != OK) {
        syslog(LOG_ERR, "[%s] voice submit: %s\n", TAG, reply);
    }
}

int remote_ctrl_channel_start(void)
{
    int ret;

    if (!g_configured) {
        return OK;
    }

    if (g_started) {
        return OK;
    }

    ret = agent_task_create(transport_task, "remote_ctrl",
        AGENT_REMOTE_CTRL_STACK, NULL, AGENT_REMOTE_CTRL_PRIO);
    if (ret != OK) {
        syslog(LOG_ERR, "[%s] failed to create transport thread\n", TAG);
        return ERROR;
    }

    g_started = true;

    /* Registering here, not in init(), is what keeps this optional: an
     * unconfigured channel never claims the voice text, so a board that
     * only wants voice chat keeps the chat-bus default untouched.
     */

    voice_channel_register_sink(voice_sink_to_remote, NULL);
    syslog(LOG_INFO, "[%s] started (voice sink registered)\n", TAG);
    return OK;
}

void remote_ctrl_channel_stop(void)
{
    if (!g_started) {
        return;
    }

    remote_link_mqtt_stop();
    g_started = false;

    /* Release anyone blocked in remote_ctrl_channel_prompt(). */
    pthread_mutex_lock(&g_turn_lock);
    g_turn_finished = true;
    pthread_cond_broadcast(&g_turn_cond);
    pthread_mutex_unlock(&g_turn_lock);

    syslog(LOG_INFO, "[%s] stop requested\n", TAG);
}

bool remote_ctrl_channel_enabled(void)
{
    return g_configured;
}

/* ── Operations ────────────────────────────────────────────────────── */

int remote_ctrl_channel_status(struct remote_status_s* out)
{
    if (out == NULL || !g_configured) {
        return ERROR;
    }

    remote_session_status(&g_session, out);
    return OK;
}

int remote_ctrl_channel_query_usage(void)
{
    if (!g_configured) {
        return ERROR;
    }

    return remote_session_query_usage(&g_session) ? OK : ERROR;
}

int remote_ctrl_channel_model_view(struct remote_model_view_s* out)
{
    if (out == NULL || !g_configured) {
        return ERROR;
    }

    remote_session_model_view(&g_session, out);
    return OK;
}

int remote_ctrl_channel_select_model(const char* model)
{
    if (model == NULL || !g_configured) {
        return ERROR;
    }

    return remote_session_select_model(&g_session, model) ? OK : ERROR;
}

int remote_ctrl_channel_set_mode(const char* mode)
{
    if (mode == NULL || !g_configured) {
        return ERROR;
    }

    return remote_session_set_mode(&g_session, mode) ? OK : ERROR;
}

int remote_ctrl_channel_request_sessions(void)
{
    if (!g_configured) {
        return ERROR;
    }

    return remote_session_request_sessions(&g_session) ? OK : ERROR;
}

int remote_ctrl_channel_new_session(void)
{
    if (!g_configured) {
        return ERROR;
    }

    return remote_session_new_session(&g_session) ? OK : ERROR;
}

int remote_ctrl_channel_switch_session(const char* target)
{
    if (target == NULL || !g_configured) {
        return ERROR;
    }

    return remote_session_switch_session(&g_session, target) ? OK : ERROR;
}

int remote_ctrl_channel_session_view(char* list_out, size_t list_size,
    char* current_out, size_t current_size)
{
    if (list_out == NULL || list_size == 0 || !g_configured) {
        return ERROR;
    }

    pthread_mutex_lock(&g_session.lock);
    snprintf(list_out, list_size, "%s", g_session.session_list_text);
    if (current_out != NULL && current_size > 0) {
        snprintf(current_out, current_size, "%s", g_session.current_session);
    }
    pthread_mutex_unlock(&g_session.lock);
    return OK;
}

int remote_ctrl_channel_decide(enum remote_decision_e decision)
{
    if (!g_configured) {
        return ERROR;
    }

    return remote_session_decide(&g_session, decision) ? OK : ERROR;
}

/* The entry points handed to an input adapter.  They keep the console
 * feedback here so every source -- keys, a touch widget, a voice intent --
 * reports the same way, and so the adapter never has to know about the
 * channel's configuration or session state.
 */

static int remote_ctrl_pending_from_input(void *ctx)
{
    (void)ctx;
    struct remote_status_s st;

    if (g_debug_pending)
        return 1;

    if (!g_configured)
        return 0;

    if (remote_ctrl_channel_status(&st) != OK)
        return 0;

    return st.has_prompt ? 1 : 0;
}

static void remote_ctrl_decide_from_input(enum remote_decision_e decision,
                                          void *ctx)
{
    (void)ctx;

    if (g_debug_pending) {
        /* A simulated approval (rpending): answer it locally.  There is no
         * real prompt in the session, so remote_ctrl_channel_decide() would
         * only report "nothing pending". */
        g_debug_pending = false;
#if defined(CONFIG_AI_AGENT_LVGL_UI) || defined(CONFIG_AI_AGENT_UI_RPMSG)
        lvgl_ui_channel_clear_approval();
        lvgl_toast_show_async("审批",
                              decision == REMOTE_DECISION_DENY ? "已取消"
                                                               : "已通过",
                              decision == REMOTE_DECISION_DENY ? 0xeb5952
                                                               : 0x4caf50,
                              2000);
#endif
        printf(decision == REMOTE_DECISION_DENY ? "Denied (debug).\n"
                                                : "Approved once (debug).\n");
        return;
    }

    if (remote_ctrl_channel_decide(decision) != OK) {
        printf("No approvable request is pending (or one was already "
               "answered).\n");
        return;
    }

    printf(decision == REMOTE_DECISION_DENY ? "Denied.\n"
                                            : "Approved once.\n");
}

static void remote_ctrl_scroll_from_input(int dir, void *ctx)
{
    (void)ctx;
#if defined(CONFIG_AI_AGENT_LVGL_UI) || defined(CONFIG_AI_AGENT_UI_RPMSG)
    lvgl_ui_channel_scroll(dir);
#else
    (void)dir;
#endif
}

static void remote_ctrl_model_next_from_input(void *ctx)
{
    (void)ctx;
    /* Larger than the 4 KB key-thread stack: keep it static. */
    static struct remote_model_view_s view;
    unsigned int i;
    unsigned int next;

    if (remote_ctrl_channel_model_view(&view) != OK || view.count == 0) {
        printf("No model list to cycle (channel unconfigured?).\n");
        return;
    }

    for (i = 0; i < view.count; i++) {
        if (strcmp(view.options[i], view.current) == 0)
            break;
    }
    next = (i == view.count ? 0 : (i + 1) % view.count);

    if (remote_ctrl_channel_select_model(view.options[next]) != OK) {
        printf("Model switch request failed.\n");
        return;
    }
    printf("Model -> %s\n", view.options[next]);
}

static void remote_ctrl_session_new_from_input(void *ctx)
{
    (void)ctx;

    if (remote_ctrl_channel_new_session() != OK) {
        printf("New-session request failed (channel unconfigured?).\n");
        return;
    }
    printf("New session requested.\n");
}

/* Wake intent: while the standby clock is up, a key press only unlocks the
 * screen and must be swallowed — the S2/S3 that wakes the device must not also
 * answer a pending approval (or scroll). Returns 1 when it consumed the press
 * as a wake, 0 when the screen was already awake and the press should dispatch. */
static int remote_ctrl_wake_from_input(void *ctx)
{
    (void)ctx;
#if defined(CONFIG_AI_AGENT_LVGL_UI) || defined(CONFIG_AI_AGENT_UI_RPMSG)
    return lvgl_ui_channel_wake_from_standby();
#else
    return 0;
#endif
}

int remote_ctrl_register_input(const struct remote_input_ops_s *ops)
{
    if (ops == NULL || ops->attach == NULL) {
        return ERROR;
    }

    return ops->attach(remote_ctrl_decide_from_input,
                       remote_ctrl_pending_from_input,
                       remote_ctrl_scroll_from_input,
                       remote_ctrl_model_next_from_input,
                       remote_ctrl_session_new_from_input,
                       remote_ctrl_wake_from_input,
                       NULL);
}

void remote_ctrl_channel_debug_pending(const char *tool, const char *hint)
{
    g_debug_pending = true;
#if defined(CONFIG_AI_AGENT_LVGL_UI) || defined(CONFIG_AI_AGENT_UI_RPMSG)
    lvgl_ui_channel_set_approval(tool != NULL ? tool : "remote-tool",
                                 hint != NULL ? hint : "");
#endif
    printf("Simulated approval pending: %s\n",
           tool != NULL ? tool : "remote-tool");
}

static const struct remote_observer_ops_s *g_observer;

int remote_ctrl_register_observer(const struct remote_observer_ops_s *ops)
{
    if (ops == NULL || ops->on_state == NULL) {
        return ERROR;
    }

    g_observer = ops;
    return OK;
}

static void notify_observer_state(const char *text)
{
    if (g_observer != NULL && g_observer->on_state != NULL) {
        g_observer->on_state(text, g_observer->ctx);
    }
}

static void notify_observer_reply(const char *text)
{
    if (g_observer != NULL && g_observer->on_reply != NULL) {
        g_observer->on_reply(text, g_observer->ctx);
    }
}

/* Turns a refused submit into something a person or a model can act on. The
 * session refuses for several distinct reasons and they need different
 * responses, so "failed" on its own would be useless. */
static void describe_submit_failure(const char* text, char* out,
    size_t out_size)
{
    struct remote_status_s status;
    char problem[192];

    remote_session_status(&g_session, &status);

    if (status.state == REMOTE_STATE_OFFLINE) {
        snprintf(out, out_size, "Error: remote agent link is offline");
    } else if (status.has_prompt) {
        snprintf(out, out_size,
            "Error: the remote agent is waiting for a permission decision on "
            "this device; answer it before sending a new prompt");
    } else if (status.turn_active) {
        snprintf(out, out_size, "Error: a remote turn is already running");
    } else if (!status.chat_supported) {
        snprintf(out, out_size,
            "Error: the remote bridge does not offer chat turns");
    } else if (!status.chat_epoch_current) {
        /* remote_link_can_submit_prompt() also requires chat_epoch == epoch.
         * A bridge reconnect refreshes epoch but leaves chat_epoch on the
         * old value until the next chat advertisement, and the turn was
         * refused with the generic "rejected locally" until this case had
         * a name of its own.
         */

        snprintf(out, out_size,
            "Error: the chat channel is not bound to this connection yet "
            "(bridge reconnected?); try again in a moment");
    } else if (remote_prompt_text_problem(text, problem, sizeof(problem))) {
        /* Remaining refusals are about the text itself.  Name the check it
         * failed: "empty or too long or bad UTF-8" sent people looking at
         * the length of a 32-byte prompt that had lost a byte on the
         * console.
         */

        snprintf(out, out_size, "Error: %s", problem);
    } else {
        snprintf(out, out_size, "Error: the prompt was refused locally");
    }
}

int remote_ctrl_channel_submit(const char* text, char* out, size_t out_size)
{
    if (text == NULL || out == NULL || out_size == 0) {
        return ERROR;
    }

    if (!g_configured) {
        snprintf(out, out_size, "Error: remote control is not configured");
        return ERROR;
    }

    /* Deliberately does not touch the blocking-wait state: the session's own
     * turn reservation is what keeps two turns from overlapping, and nothing
     * would clear a wait that no one is performing. */
    if (!remote_session_submit_prompt(&g_session, text)) {
        describe_submit_failure(text, out, out_size);
        return ERROR;
    }
    /* The submit succeeded, so the turn that is starting belongs to rask:
     * mirror its transcript onto the chat surfaces when it lands. */
    g_mirror_turn = true;

    snprintf(out, out_size,
        "Prompt sent. Use 'rstat' for progress; the reply prints in one piece "
        "when the turn ends, and 'rreply' re-reads it.");
    return OK;
}

int remote_ctrl_channel_transcript(char* out, size_t out_size)
{
    if (out == NULL || out_size == 0 || !g_configured) {
        return ERROR;
    }

    return remote_session_transcript(&g_session, out, out_size) ? OK : ERROR;
}

int remote_ctrl_channel_prompt(const char* text, uint32_t timeout_ms,
    char* out, size_t out_size)
{
    struct timespec deadline;
    bool finished;
    int ret = OK;

    if (text == NULL || out == NULL || out_size == 0) {
        return ERROR;
    }

    if (!g_configured) {
        snprintf(out, out_size, "Error: remote control is not configured");
        return ERROR;
    }

    /* Only one caller may own the turn slot at a time. The session would refuse
     * a second submit anyway; rejecting here keeps the waiters unambiguous. */
    pthread_mutex_lock(&g_turn_lock);
    if (g_turn_pending) {
        pthread_mutex_unlock(&g_turn_lock);
        snprintf(out, out_size, "Error: a remote turn is already in progress");
        return ERROR;
    }
    /* Armed before submitting so a turn that completes immediately cannot be
     * missed between the submit and the wait. */
    g_turn_pending = true;
    g_turn_finished = false;
    /* The LLM presents its own answer built from the tool result; a mirrored
     * raw transcript on top of that would say everything twice. */
    g_mirror_turn = false;
    pthread_mutex_unlock(&g_turn_lock);

    if (!remote_session_submit_prompt(&g_session, text)) {
        pthread_mutex_lock(&g_turn_lock);
        g_turn_pending = false;
        pthread_mutex_unlock(&g_turn_lock);

        describe_submit_failure(text, out, out_size);
        return ERROR;
    }

    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec += 1;
        deadline.tv_nsec -= 1000000000L;
    }

    pthread_mutex_lock(&g_turn_lock);
    while (!g_turn_finished) {
        if (pthread_cond_timedwait(&g_turn_cond, &g_turn_lock, &deadline) == ETIMEDOUT) {
            break;
        }
    }
    finished = g_turn_finished;
    g_turn_pending = false;
    pthread_mutex_unlock(&g_turn_lock);

    if (!remote_session_transcript(&g_session, out, out_size)) {
        snprintf(out, out_size,
            "Error: remote agent output does not fit the tool buffer");
        return ERROR;
    }

    if (!finished) {
        struct remote_status_s status;

        remote_session_status(&g_session, &status);

        /* A turn stalled on a permission request is an expected outcome, not a
         * failure: only a human on this device can release it. */
        if (status.has_prompt) {
            size_t used = strlen(out);

            snprintf(out + used, out_size - used,
                "%s[The remote agent is waiting for approval of %s on this "
                "device. It cannot continue until someone answers.]",
                used > 0 ? "\n" : "", status.prompt.tool);
        } else {
            size_t used = strlen(out);

            snprintf(out + used, out_size - used,
                "%s[The remote turn is still running; output so far is above.]",
                used > 0 ? "\n" : "");
        }
        ret = OK;
    }

    if (out[0] == '\0') {
        snprintf(out, out_size, "The remote agent produced no text output.");
    }

    return ret;
}

#endif /* CONFIG_AI_AGENT_REMOTE_CTRL */
