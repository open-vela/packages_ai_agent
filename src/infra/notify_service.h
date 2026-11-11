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

/**
 * notify_service.h — Gerrit/Jira background polling → LVGL toast.
 *
 * A detached polling thread periodically queries connected MCP servers
 * (gerrit.query_changes / jira.search) for recent changes, deduplicates
 * against a persisted cursor in config_store, and for each genuinely new
 * item pushes a message on the bus (channel "lvgl_notify") which the
 * outbound dispatcher turns into a top-of-screen LVGL toast.
 *
 * It runs OUTSIDE the agent_loop ReAct loop — polling is a plain sync
 * MCP tool call on the notify thread, and notification text is a fast
 * snprintf template (no LLM by default).  This keeps the agent chat loop
 * responsive and avoids spending tokens on empty polls.
 *
 * Lifecycle mirrors cron_service/heartbeat: init() loads config + cursor
 * (no thread), start() spawns the thread, stop() clears the running flag.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    NOTIFY_SRC_GERRIT = 0,
    NOTIFY_SRC_JIRA,
    NOTIFY_SRC_COUNT
} notify_source_t;

/**
 * Initialize the notify service: load per-source config and cursors from
 * config_store into memory.  Does NOT start the thread.
 * Call once at startup (Phase 3), after mcp_client_init().
 */
int notify_service_init(void);

/**
 * Start the polling thread.  Idempotent — returns OK if already running.
 * Call after network is up (the thread issues sync HTTP MCP calls).
 */
int notify_service_start(void);

/**
 * Stop the polling thread.  Sets running=false and signals the cond so the
 * thread wakes immediately.  The thread is detached and exits on its own.
 */
void notify_service_stop(void);

/**
 * Build a JSON status string for CLI display (caller frees).
 * Format: {"running":bool,"last_poll":N,"last_new":N,"last_err":"...",
 *          "sources":[{"name":"gerrit","enabled":bool,"cursor":"...",
 *                       "interval":N}, ...]}
 */
char* notify_service_status_json(void);

/**
 * Inject a notification bypassing the poll (debug aid).
 * Pops a toast immediately via the same outbound path.
 */
int notify_service_inject(notify_source_t src, const char *key,
                          const char *summary);

#ifdef __cplusplus
}
#endif
