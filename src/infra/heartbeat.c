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
#include <errno.h>
#include <unistd.h>

static const char *TAG = "heartbeat";

#define HEARTBEAT_PROMPT \
    "Do not read HEARTBEAT.md or skill files. " \
    "Do not write /data/velaguard/reports/runtime-report.md " \
    "(firmware already wrote the screen report). " \
    "If read_file /data/velaguard/pending_alarm.txt succeeds, " \
    "write last_alarm.md using vgstats/vgmodbus/vgcfg dump. " \
    "Never list_dir, ls, or cat. If no pending alarm, reply HEARTBEAT_OK."

static volatile bool s_heartbeat_running = false;

/* ── Content check ────────────────────────────────────────────── */

#ifndef CONFIG_VG_HMI
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
#endif

/* ── Send heartbeat to agent ──────────────────────────────────── */

#ifndef VG_PENDING_ALARM_PATH
#define VG_PENDING_ALARM_PATH "/data/velaguard/pending_alarm.txt"
#endif

static bool heartbeat_send(bool force)
{
#ifdef CONFIG_VG_HMI
    /* HMI must not start ReAct from the timer, leftover HEARTBEAT.poke,
     * report-page poke, or NSH heartbeat_trigger. 2026-09-14 15:19:
     * system prompt built (3489 B) then mm_forcefree IMPRECISERR in
     * ai_agent and NSH died. Screen report is firmware-local. */
    (void)force;
    syslog(LOG_INFO, "[%s] skip LLM (HMI, no ReAct from heartbeat/poke)\n", TAG);
    return false;
#else
    if (!force && !heartbeat_has_tasks()) {
        return false;
    }
#endif

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


/* ── Poke file ────────────────────────────────────────────────── */

/* AGENT_HEARTBEAT_POKE_FILE present -> run a check right away. External
 * tasks (e.g. on-demand daily report) touch it instead of calling into
 * the agent process, which may not have initialized its message bus yet. */
static bool heartbeat_poke_consumed(void)
{
    if (access(AGENT_HEARTBEAT_POKE_FILE, F_OK) != 0) {
        return false;
    }

    if (unlink(AGENT_HEARTBEAT_POKE_FILE) != 0 && errno != ENOENT) {
        syslog(LOG_WARNING, "[%s] Failed to unlink poke file: %s\n", TAG,
               strerror(errno));
    }
    return true;
}

static void *heartbeat_thread(void *arg)
{
    (void)arg;

    while (s_heartbeat_running) {
        /* Slice the interval sleep so a poke file triggers an immediate
         * check; also makes heartbeat_stop() responsive. */
        int interval_s = AGENT_HEARTBEAT_INTERVAL_MS / 1000;
        int waited = 0;
        bool poked = false;

        while (s_heartbeat_running && waited < interval_s) {
            sleep(AGENT_HEARTBEAT_POLL_SLICE_S);
            waited += AGENT_HEARTBEAT_POLL_SLICE_S;
            if (heartbeat_poke_consumed()) {
                poked = true;
                break;
            }
        }

        if (!s_heartbeat_running) {
            break;
        }

        heartbeat_send(poked);
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
    return heartbeat_send(true);
}
