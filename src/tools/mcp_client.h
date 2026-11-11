/*
 * Copyright (C) 2025 Xiaomi Corporation
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

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum remote MCP servers we can connect to */
#ifdef CONFIG_AI_AGENT_MCP_MAX_SERVERS
#define MCP_CLIENT_MAX_SERVERS CONFIG_AI_AGENT_MCP_MAX_SERVERS
#else
#define MCP_CLIENT_MAX_SERVERS 4
#endif

/* Maximum cached remote tools across all servers */
#ifdef CONFIG_AI_AGENT_MCP_MAX_TOOLS
#define MCP_CLIENT_MAX_TOOLS CONFIG_AI_AGENT_MCP_MAX_TOOLS
#else
#define MCP_CLIENT_MAX_TOOLS 16
#endif

/**
 * Add a remote MCP server endpoint.
 *
 * @param name   Friendly name (e.g. "amap")
 * @param url    Streamable HTTP endpoint (e.g. "http://host:port/mcp")
 * @param token  Optional bearer token for auth, or NULL
 * @return OK on success, ERROR on failure
 */
int mcp_client_add_server(const char* name, const char* url,
    const char* token, const char* jira_url, const char* jira_user,
    const char* jira_pat);

/**
 * Remove a remote MCP server by name.
 */
int mcp_client_remove_server(const char* name);

/**
 * Initialize the MCP client module. Call once at startup.
 */
int mcp_client_init(void);

/**
 * Discover tools from all registered remote servers.
 * Sends initialize + tools/list to each server and caches results.
 *
 * @return number of tools discovered, or -1 on error
 */
int mcp_client_discover(void);

/**
 * Try to execute a tool via remote MCP servers.
 *
 * @param name        Tool name
 * @param input_json  Arguments JSON string
 * @param output      Output buffer (caller-owned)
 * @param output_size Size of output buffer
 * @return OK if tool was found and executed, ERROR if not found
 */
int mcp_client_execute(const char* name, const char* input_json,
    char* output, size_t output_size);

/**
 * Get remote tools as a JSON array string matching AI Agent format:
 *   [{"name":"...", "description":"...", "input_schema":{...}}, ...]
 *
 * Caller must free() the returned string. Returns NULL if no tools.
 */
char* mcp_client_get_tools_json(void);

/**
 * Get number of registered remote servers.
 */
int mcp_client_server_count(void);

/**
 * Get status info as a JSON string for CLI display.
 * Caller must free().
 */
char* mcp_client_status_json(void);

/**
 * Cleanup all remote connections and cached data.
 */
void mcp_client_cleanup(void);

/**
 * Persist current server list to config_store and reload from it.
 *
 * persist_save() writes every registered server's connection params
 * (name/url/token/jira_url/jira_user/jira_pat) so they survive reboot.
 * persist_load() reads them back, re-adds each server, and re-discovers
 * tools — call once at the end of mcp_client_init() for auto-reconnect.
 *
 * Both are no-ops (return OK) if CONFIG_AI_AGENT_MCP_SERVER_PERSIST is off.
 */
int mcp_client_persist_save(void);
int mcp_client_persist_load(void);

#ifdef __cplusplus
}
#endif
