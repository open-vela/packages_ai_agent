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

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include "infra/heartbeat.h"
#include "agent_config.h"
#include "core/agent_loop.h"
#include "core/message_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>

static const char *TAG = "heartbeat";

#define HEARTBEAT_PROMPT \
    "Read " AGENT_HEARTBEAT_FILE " and follow any instructions or tasks listed there. " \
    "If nothing needs attention, reply with just: HEARTBEAT_OK"

static volatile bool s_heartbeat_running = false;

/* ── Content check ────────────────────────────────────────────── */

/**
 * Check if HEARTBEAT.md has actionable content.
 * Returns true if any line is NOT:
 *   - empty / whitespace-only
 *   - a markdown header (starts with #)
 *   - a completed checkbox (- [x] or * [x])
 */
static bool heartbeat_has_tasks(void)
{
    FILE *f = fopen(AGENT_HEARTBEAT_FILE, "r");
    if (!f) {
        return false;
    }

    char line[256];
    bool found_task = false;

    while (fgets(line, sizeof(line), f)) {
        /* Skip leading whitespace */
        const char *p = line;
        while (*p && isspace((unsigned char)*p)) {
            p++;
        }

        /* Skip empty lines */
        if (*p == '\0') {
            continue;
        }

        /* Skip markdown headers */
        if (*p == '#') {
            continue;
        }

        /* Skip completed checkboxes: "- [x]" or "* [x]" */
        if ((*p == '-' || *p == '*') && *(p + 1) == ' ' && *(p + 2) == '[') {
            char mark = *(p + 3);
            if ((mark == 'x' || mark == 'X') && *(p + 4) == ']') {
                continue;
            }
        }

        /* Found an actionable line */
        found_task = true;
        break;
    }

    fclose(f);
    return found_task;
}

/* ── Send heartbeat to agent ──────────────────────────────────── */

static bool heartbeat_send(void)
{
    if (!heartbeat_has_tasks()) {
        return false;
    }

    agent_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, AGENT_CHAN_SYSTEM, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "heartbeat", sizeof(msg.chat_id) - 1);
    msg.content = strdup(HEARTBEAT_PROMPT);

    if (!msg.content) {
        syslog(LOG_ERR, "[%s] Failed to allocate heartbeat prompt\n", TAG);
        return false;
    }

    int err = message_bus_push_inbound(&msg);
    if (err != OK) {
        syslog(LOG_WARNING, "[%s] Failed to push heartbeat message: %s\n", TAG, strerror(errno));
        free(msg.content);
        return false;
    }

    syslog(LOG_INFO, "[%s] Triggered agent check\n", TAG);

    /* Lazy-loop builds (VelaGuard HMI) only start agent_loop on demand;
     * make sure the queued heartbeat prompt actually gets consumed. */
    agent_loop_ensure_started();
    return true;
}


static void *heartbeat_thread(void *arg)
{
    (void)arg;

    while (s_heartbeat_running) {
        /* Sleep for the heartbeat interval (use sleep() to avoid usleep overflow) */
        sleep(AGENT_HEARTBEAT_INTERVAL_MS / 1000);
        heartbeat_send();
    }

    return NULL;
}

/* ── Public API ───────────────────────────────────────────────── */

int heartbeat_init(void)
{
    syslog(LOG_INFO, "[%s] Heartbeat service initialized (file: %s, interval: %ds)\n", TAG, AGENT_HEARTBEAT_FILE, AGENT_HEARTBEAT_INTERVAL_MS / 1000);
    return OK;
}

int heartbeat_start(void)
{
    if (s_heartbeat_running) {
        syslog(LOG_WARNING, "[%s] Heartbeat timer already running\n", TAG);
        return OK;
    }

    s_heartbeat_running = true;

    int err = agent_task_create(
        heartbeat_thread,
        "heartbeat",
        4096,
        NULL,
        3
    );

    if (err != OK) {
        s_heartbeat_running = false;
        syslog(LOG_ERR, "[%s] Failed to create heartbeat thread\n", TAG);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Heartbeat started (every %d min)\n", TAG, AGENT_HEARTBEAT_INTERVAL_MS / 60000);
    return OK;
}

void heartbeat_stop(void)
{
    if (s_heartbeat_running) {
        s_heartbeat_running = false;
        /* Thread will exit on next iteration */
        syslog(LOG_INFO, "[%s] Heartbeat stopped\n", TAG);
    }
}

bool heartbeat_trigger(void)
{
    return heartbeat_send();
}
