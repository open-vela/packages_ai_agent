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

#ifndef __A2A_HANDLER_H__
#define __A2A_HANDLER_H__

#include <stdbool.h>
#include <stddef.h>

/**
 * a2a_handler.h — A2A protocol handler (Server + Client).
 *
 * Server role: handles incoming HTTP requests on the ws_server port.
 *   GET  /.well-known/agent.json  → AgentCard (device capabilities)
 *   POST /a2a/invoke              → Execute tool via tool_registry
 *   GET  /a2a/health              → Health check
 *
 * Client role: discovers and invokes remote A2A agents.
 *   a2a_client_discover()  → GET remote agent's AgentCard
 *   a2a_client_invoke()    → POST to remote agent's /a2a/invoke
 */

/* ── Server (called from ws_server.c) ─────────────────────── */

/**
 * Try to handle an HTTP request as A2A.
 * Called from ws_server client_thread after reading HTTP headers.
 *
 * @param fd       Client socket fd
 * @param buf      Already-read HTTP headers (NUL-terminated)
 * @param buf_len  Length of data in buf
 * @return true if handled (caller should close fd), false if not A2A
 */
bool a2a_try_handle(int fd, const char *buf, int buf_len);

/* ── Client (call from agent code / tools) ────────────────── */

/**
 * Discover a remote A2A agent's capabilities.
 *
 * @param host     Remote host (e.g. "192.168.1.100")
 * @param port     Remote port (e.g. "8080")
 * @return AgentCard JSON string (caller must free), or NULL on error
 */
char *a2a_client_discover(const char *host, const char *port);

/**
 * Invoke a skill on a remote A2A agent.
 *
 * @param host      Remote host
 * @param port      Remote port
 * @param skill_id  Skill/tool name to invoke
 * @param args_json Arguments as JSON string
 * @return Result JSON string (caller must free), or NULL on error
 */
char *a2a_client_invoke(const char *host, const char *port,
                        const char *skill_id, const char *args_json);

/**
 * Check health of a remote A2A agent.
 *
 * @param host  Remote host
 * @param port  Remote port
 * @return 0 if healthy, <0 on error
 */
int a2a_client_health(const char *host, const char *port);

#endif /* __A2A_HANDLER_H__ */
