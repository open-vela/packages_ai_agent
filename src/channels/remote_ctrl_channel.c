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
#include "infra/config_store.h"
#include "remote/remote_link_mqtt.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
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

/* ── Event handler ─────────────────────────────────────────────────── */

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
         * arrives as REMOTE_EVENT_TURN_TRANSCRIPT once the turn ends. */
        syslog(LOG_INFO, "[%s] status: %s\n", TAG, text != NULL ? text : "");
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
        syslog(LOG_INFO, "[%s] reply:\n%s", TAG, text != NULL ? text : "");
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
    syslog(LOG_INFO, "[%s] started\n", TAG);
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

int remote_ctrl_channel_decide(enum remote_decision_e decision)
{
    if (!g_configured) {
        return ERROR;
    }

    return remote_session_decide(&g_session, decision) ? OK : ERROR;
}

/* Turns a refused submit into something a person or a model can act on. The
 * session refuses for several distinct reasons and they need different
 * responses, so "failed" on its own would be useless. */
static void describe_submit_failure(char* out, size_t out_size)
{
    struct remote_status_s status;

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
    } else {
        snprintf(out, out_size, "Error: prompt rejected locally");
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
        describe_submit_failure(out, out_size);
        return ERROR;
    }

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
    pthread_mutex_unlock(&g_turn_lock);

    if (!remote_session_submit_prompt(&g_session, text)) {
        pthread_mutex_lock(&g_turn_lock);
        g_turn_pending = false;
        pthread_mutex_unlock(&g_turn_lock);

        describe_submit_failure(out, out_size);
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
