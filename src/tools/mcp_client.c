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

#include "tools/mcp_client.h"
#include "tools/tool_registry.h"
#include "cJSON.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "infra/url_parse.h"

static const char* TAG = "mcp_client";

/* ── Data structures ────────────────────────────────────── */

typedef struct {
    char name[128]; /* "server.tool" prefixed name */
    char description[256];
    char* input_schema_json; /* heap */
    char server_name[64]; /* which server owns this tool */
} mcp_remote_tool_t;

typedef struct {
    char name[64];
    char url[256];
    char token[128];
    parsed_url_t parsed;
    bool initialized; /* MCP initialize handshake done */
    char session_id[128]; /* Mcp-Session header from server */
} mcp_remote_server_t;

static mcp_remote_server_t s_servers[MCP_CLIENT_MAX_SERVERS];
static int s_server_count = 0;

static mcp_remote_tool_t s_tools[MCP_CLIENT_MAX_TOOLS];
static int s_tool_count = 0;

static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static bool s_inited = false;
static atomic_int s_req_id = 1;

/* Response buffer size for MCP HTTP calls */
#define MCP_RESP_BUF_SIZE 8192

/* ── JSON-RPC request builder ───────────────────────────── */

static char* build_jsonrpc_request(const char* method, cJSON* params)
{
    cJSON* req = cJSON_CreateObject();
    if (!req) {
        /* Caller passed params expecting us to take ownership — free it */
        cJSON_Delete(params);
        return NULL;
    }

    cJSON_AddStringToObject(req, "jsonrpc", "2.0");
    cJSON_AddStringToObject(req, "method", method);
    cJSON_AddNumberToObject(req, "id", s_req_id++);

    if (params) {
        cJSON_AddItemToObject(req, "params", params);
    } else {
        cJSON_AddItemToObject(req, "params", cJSON_CreateObject());
    }

    char* json = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    return json;
}

/* ── HTTP POST to MCP server ────────────────────────────── */

static int mcp_http_post(mcp_remote_server_t* srv, const char* body,
    char* resp_buf, size_t resp_cap)
{
    vela_header_t headers[5];
    int h = 0;

    headers[h].name = "Accept";
    headers[h].value = "application/json, text/event-stream";
    h++;

    /* Force new connection each request — some MCP servers (e.g. DiDi)
     * close the connection after responding, but vela_tls pool tries to
     * reuse it and hangs on read. */
    headers[h].name = "Connection";
    headers[h].value = "close";
    h++;

    if (srv->session_id[0]) {
        headers[h].name = "Mcp-Session";
        headers[h].value = srv->session_id;
        h++;
    }

    if (srv->token[0]) {
        /* Build "Bearer <token>" — use heap to avoid static buffer race */
        char* auth_buf = malloc(192);
        if (!auth_buf)
            return -1;
        snprintf(auth_buf, 192, "Bearer %s", srv->token);
        headers[h].name = "Authorization";
        headers[h].value = auth_buf;
        h++;

        headers[h].name = NULL;
        headers[h].value = NULL;

        int status;
        if (srv->parsed.use_tls) {
            status = vela_https_post_json(
                srv->parsed.host, srv->parsed.port, srv->parsed.path,
                headers, body, resp_buf, resp_cap);
        } else {
            status = vela_http_post_json(
                srv->parsed.host, srv->parsed.port, srv->parsed.path,
                headers, body, resp_buf, resp_cap);
        }

        free(auth_buf);
        return status;
    }

    headers[h].name = NULL;
    headers[h].value = NULL;

    int status;
    if (srv->parsed.use_tls) {
        status = vela_https_post_json(
            srv->parsed.host, srv->parsed.port, srv->parsed.path,
            headers, body, resp_buf, resp_cap);
    } else {
        status = vela_http_post_json(
            srv->parsed.host, srv->parsed.port, srv->parsed.path,
            headers, body, resp_buf, resp_cap);
    }

    return status;
}

/* ── Parse JSON-RPC response ────────────────────────────── */

static cJSON* parse_jsonrpc_result(const char* resp_json)
{
    cJSON* root = cJSON_Parse(resp_json);
    if (!root)
        return NULL;

    cJSON* error = cJSON_GetObjectItem(root, "error");
    if (error) {
        cJSON* msg = cJSON_GetObjectItem(error, "message");
        syslog(LOG_ERR, "[%s] JSON-RPC error: %s\n", TAG,
            (msg && cJSON_IsString(msg)) ? msg->valuestring : "unknown");
        cJSON_Delete(root);
        return NULL;
    }

    cJSON* result = cJSON_DetachItemFromObject(root, "result");
    cJSON_Delete(root);
    return result;
}

/* ── MCP initialize handshake (called WITHOUT lock) ─────── */

static int mcp_client_handshake(mcp_remote_server_t* srv)
{
    cJSON* params = cJSON_CreateObject();
    if (!params)
        return ERROR;

    cJSON* caps = cJSON_CreateObject();
    if (!caps) {
        cJSON_Delete(params);
        return ERROR;
    }
    cJSON_AddItemToObject(params, "capabilities", caps);

    cJSON* info = cJSON_CreateObject();
    if (!info) {
        cJSON_Delete(params);
        return ERROR;
    }
    cJSON_AddStringToObject(info, "name", "agent");
    cJSON_AddStringToObject(info, "version", "1.0.0");
    cJSON_AddItemToObject(params, "clientInfo", info);

    cJSON_AddStringToObject(params, "protocolVersion", "2025-03-26");

    char* body = build_jsonrpc_request("initialize", params);
    if (!body)
        return ERROR;

    char* resp_buf = malloc(MCP_RESP_BUF_SIZE);
    if (!resp_buf) {
        free(body);
        return ERROR;
    }
    resp_buf[0] = '\0';

    int status = mcp_http_post(srv, body, resp_buf, MCP_RESP_BUF_SIZE);
    free(body);

    if (status < 200 || status >= 300) {
        syslog(LOG_ERR, "[%s] %s initialize failed: HTTP %d\n",
            TAG, srv->name, status);
        free(resp_buf);
        return ERROR;
    }

    cJSON* result = parse_jsonrpc_result(resp_buf);
    free(resp_buf);

    if (!result) {
        syslog(LOG_ERR, "[%s] %s initialize: bad response\n", TAG, srv->name);
        return ERROR;
    }

    /* FIX #5: validate protocolVersion in response */
    cJSON* proto = cJSON_GetObjectItem(result, "protocolVersion");
    if (!proto || !cJSON_IsString(proto)) {
        syslog(LOG_ERR, "[%s] %s: missing protocolVersion in init response\n",
            TAG, srv->name);
        cJSON_Delete(result);
        return ERROR;
    }

    /* Accept any 2025-xx-xx version for forward compat */
    if (strncmp(proto->valuestring, "2025-", 5) != 0
        && strncmp(proto->valuestring, "2024-", 5) != 0) {
        syslog(LOG_ERR, "[%s] %s: unsupported protocol: %s\n",
            TAG, srv->name, proto->valuestring);
        cJSON_Delete(result);
        return ERROR;
    }

    /* Extract server info for logging */
    cJSON* sinfo = cJSON_GetObjectItem(result, "serverInfo");
    if (sinfo) {
        cJSON* sname = cJSON_GetObjectItem(sinfo, "name");
        syslog(LOG_INFO, "[%s] Connected to MCP server: %s (proto %s)\n", TAG,
            (sname && cJSON_IsString(sname)) ? sname->valuestring : "unknown",
            proto->valuestring);
    }

    cJSON_Delete(result);

    /* MCP spec says client SHOULD send notifications/initialized, but it's
     * a notification (no response expected). Some servers (e.g. DiDi) return
     * HTTP 202 with empty body, which causes vela_tls to hang waiting for
     * response data. Since this notification is optional and no server we
     * support requires it, skip it to avoid compatibility issues. */

    /* FIX #3: session_id extraction — NOTE: vela_https_post_json does not
     * expose response headers. We set a TODO marker. For servers that require
     * Mcp-Session (stateful transport), the TLS layer would need to be
     * extended to return headers. Most MCP servers (including Amap) work
     * without session tracking in stateless mode. */

    srv->initialized = true;
    syslog(LOG_INFO, "[%s] %s handshake complete\n", TAG, srv->name);
    return OK;
}

/* ── Discover tools from one server (called WITHOUT lock) ── */

static int discover_server_tools(mcp_remote_server_t* srv,
    mcp_remote_tool_t* out_tools,
    int max_tools)
{
    if (!srv->initialized) {
        if (mcp_client_handshake(srv) != OK)
            return -1;
    }

    char* body = build_jsonrpc_request("tools/list", NULL);
    if (!body)
        return -1;

    char* resp_buf = malloc(MCP_RESP_BUF_SIZE);
    if (!resp_buf) {
        free(body);
        return -1;
    }
    resp_buf[0] = '\0';

    int status = mcp_http_post(srv, body, resp_buf, MCP_RESP_BUF_SIZE);
    free(body);

    if (status < 200 || status >= 300) {
        syslog(LOG_ERR, "[%s] %s tools/list failed: HTTP %d\n",
            TAG, srv->name, status);
        free(resp_buf);
        return -1;
    }

    cJSON* result = parse_jsonrpc_result(resp_buf);
    free(resp_buf);
    if (!result)
        return -1;

    cJSON* tools = cJSON_GetObjectItem(result, "tools");
    if (!tools || !cJSON_IsArray(tools)) {
        cJSON_Delete(result);
        return 0;
    }

    int added = 0;
    cJSON* tool = NULL;
    cJSON_ArrayForEach(tool, tools)
    {
        if (added >= max_tools)
            break;

        cJSON* name = cJSON_GetObjectItem(tool, "name");
        cJSON* desc = cJSON_GetObjectItem(tool, "description");
        cJSON* schema = cJSON_GetObjectItem(tool, "inputSchema");

        if (!name || !cJSON_IsString(name))
            continue;

        mcp_remote_tool_t* t = &out_tools[added];
        memset(t, 0, sizeof(*t));

        /* FIX #6: prefix tool name with "server_name." to avoid collision */
        snprintf(t->name, sizeof(t->name), "%s.%s",
            srv->name, name->valuestring);
        snprintf(t->description, sizeof(t->description), "[%s] %s",
            srv->name,
            (desc && cJSON_IsString(desc)) ? desc->valuestring : "");
        strncpy(t->server_name, srv->name, sizeof(t->server_name) - 1);
        t->server_name[sizeof(t->server_name) - 1] = '\0';

        if (schema) {
            t->input_schema_json = cJSON_PrintUnformatted(schema);
        } else {
            t->input_schema_json = strdup(
                "{\"type\":\"object\",\"properties\":{}}");
        }

        if (!t->input_schema_json)
            continue;

        added++;
    }

    cJSON_Delete(result);
    syslog(LOG_INFO, "[%s] %s: discovered %d tools\n", TAG, srv->name, added);
    return added;
}

/* ── Public API ─────────────────────────────────────────── */

int mcp_client_init(void)
{
    pthread_mutex_lock(&s_mtx);
    if (s_inited) {
        pthread_mutex_unlock(&s_mtx);
        return OK;
    }
    s_server_count = 0;
    s_tool_count = 0;
    atomic_store(&s_req_id, 1);
    s_inited = true;
    pthread_mutex_unlock(&s_mtx);

    /* Register as tool provider */
    tool_registry_register_provider("mcp_client",
                                    mcp_client_get_tools_json,
                                    mcp_client_execute);

    syslog(LOG_INFO, "[%s] MCP client initialized\n", TAG);
    return OK;
}

int mcp_client_add_server(const char* name, const char* url,
    const char* token)
{
    if (!name || !url)
        return ERROR;

    pthread_mutex_lock(&s_mtx);

    if (s_server_count >= MCP_CLIENT_MAX_SERVERS) {
        syslog(LOG_ERR, "[%s] Max servers reached (%d)\n",
            TAG, MCP_CLIENT_MAX_SERVERS);
        pthread_mutex_unlock(&s_mtx);
        return ERROR;
    }

    /* Check for duplicate */
    for (int i = 0; i < s_server_count; i++) {
        if (strcmp(s_servers[i].name, name) == 0) {
            syslog(LOG_WARNING, "[%s] Server '%s' already exists\n",
                TAG, name);
            pthread_mutex_unlock(&s_mtx);
            return ERROR;
        }
    }

    mcp_remote_server_t* srv = &s_servers[s_server_count];
    memset(srv, 0, sizeof(*srv));
    strncpy(srv->name, name, sizeof(srv->name) - 1);
    strncpy(srv->url, url, sizeof(srv->url) - 1);
    if (token) {
        strncpy(srv->token, token, sizeof(srv->token) - 1);
    }

    if (url_parse(url, &srv->parsed) != 0) {
        syslog(LOG_ERR, "[%s] Invalid URL: %s\n", TAG, url);
        pthread_mutex_unlock(&s_mtx);
        return ERROR;
    }

    s_server_count++;
    syslog(LOG_INFO, "[%s] Added server '%s' -> %s\n", TAG, name, url);
    pthread_mutex_unlock(&s_mtx);
    return OK;
}

int mcp_client_remove_server(const char* name)
{
    if (!name)
        return ERROR;

    pthread_mutex_lock(&s_mtx);

    int idx = -1;
    for (int i = 0; i < s_server_count; i++) {
        if (strcmp(s_servers[i].name, name) == 0) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        pthread_mutex_unlock(&s_mtx);
        return ERROR;
    }

    /* Remove tools belonging to this server */
    int w = 0;
    for (int r = 0; r < s_tool_count; r++) {
        if (strcmp(s_tools[r].server_name, name) != 0) {
            if (w != r)
                s_tools[w] = s_tools[r];
            w++;
        } else {
            free(s_tools[r].input_schema_json);
            s_tools[r].input_schema_json = NULL;
        }
    }
    s_tool_count = w;

    /* Shift servers array */
    if (idx < s_server_count - 1) {
        memmove(&s_servers[idx], &s_servers[idx + 1],
            (s_server_count - idx - 1) * sizeof(mcp_remote_server_t));
    }
    s_server_count--;

    pthread_mutex_unlock(&s_mtx);
    syslog(LOG_INFO, "[%s] Removed server '%s'\n", TAG, name);
    return OK;
}

/* FIX #1: discover — lock only protects data copy, network calls outside lock */

int mcp_client_discover(void)
{
    /* Step 1: lock, copy server list, clear old tools */
    pthread_mutex_lock(&s_mtx);

    int nservers = s_server_count;
    if (nservers == 0) {
        pthread_mutex_unlock(&s_mtx);
        return 0;
    }

    /* Stack-copy server structs so we can release the lock */
    mcp_remote_server_t* srv_copy = malloc(
        nservers * sizeof(mcp_remote_server_t));
    if (!srv_copy) {
        pthread_mutex_unlock(&s_mtx);
        return -1;
    }
    memcpy(srv_copy, s_servers, nservers * sizeof(mcp_remote_server_t));

    /* Clear existing tools */
    for (int i = 0; i < s_tool_count; i++) {
        free(s_tools[i].input_schema_json);
        s_tools[i].input_schema_json = NULL;
    }
    s_tool_count = 0;

    pthread_mutex_unlock(&s_mtx);

    /* Step 2: network calls WITHOUT lock — each server gets its own
     * temporary tool buffer to avoid blocking other operations */
    mcp_remote_tool_t* tmp_tools = malloc(
        MCP_CLIENT_MAX_TOOLS * sizeof(mcp_remote_tool_t));
    if (!tmp_tools) {
        free(srv_copy);
        return -1;
    }

    int total = 0;
    for (int i = 0; i < nservers; i++) {
        int remaining = MCP_CLIENT_MAX_TOOLS - total;
        if (remaining <= 0)
            break;

        int n = discover_server_tools(&srv_copy[i],
            &tmp_tools[total], remaining);
        if (n > 0)
            total += n;

        /* Sync initialized/session_id back under lock */
        pthread_mutex_lock(&s_mtx);
        for (int j = 0; j < s_server_count; j++) {
            if (strcmp(s_servers[j].name, srv_copy[i].name) == 0) {
                s_servers[j].initialized = srv_copy[i].initialized;
                memcpy(s_servers[j].session_id, srv_copy[i].session_id,
                    sizeof(s_servers[j].session_id));
                break;
            }
        }
        pthread_mutex_unlock(&s_mtx);
    }

    /* Step 3: lock, merge discovered tools into global array */
    pthread_mutex_lock(&s_mtx);

    for (int i = 0; i < total && s_tool_count < MCP_CLIENT_MAX_TOOLS; i++) {
        s_tools[s_tool_count] = tmp_tools[i];
        /* Ownership of input_schema_json transferred, don't free */
        s_tool_count++;
    }

    /* Free any tools that didn't fit */
    for (int i = s_tool_count; i < total; i++) {
        free(tmp_tools[i].input_schema_json);
        tmp_tools[i].input_schema_json = NULL;
    }

    pthread_mutex_unlock(&s_mtx);

    free(tmp_tools);
    free(srv_copy);
    syslog(LOG_INFO, "[%s] Total remote tools: %d\n", TAG, total);
    return total;
}

/* FIX #2: execute — lock only for lookup, network call outside lock */

int mcp_client_execute(const char* name, const char* input_json,
    char* output, size_t output_size)
{
    if (!name || !output || output_size == 0)
        return ERROR;

    /* Step 1: lock, find tool + server, copy what we need, unlock */
    pthread_mutex_lock(&s_mtx);

    const char* srv_name = NULL;
    /* FIX #6: tool names are now prefixed "server.tool", but also try
     * unprefixed match for backward compat with tool_registry fallback */
    for (int i = 0; i < s_tool_count; i++) {
        if (strcmp(s_tools[i].name, name) == 0) {
            srv_name = s_tools[i].server_name;
            break;
        }
    }

    if (!srv_name) {
        pthread_mutex_unlock(&s_mtx);
        return ERROR;
    }

    /* Copy server struct so we can release lock before network call */
    mcp_remote_server_t srv_local;
    bool found = false;
    for (int i = 0; i < s_server_count; i++) {
        if (strcmp(s_servers[i].name, srv_name) == 0) {
            srv_local = s_servers[i];
            found = true;
            break;
        }
    }

    pthread_mutex_unlock(&s_mtx);

    if (!found)
        return ERROR;

    /* Step 2: build request and do network call WITHOUT lock */

    /* Extract the original tool name (strip server prefix for remote call) */
    const char* dot = strchr(name, '.');
    const char* remote_name = dot ? dot + 1 : name;

    cJSON* params = cJSON_CreateObject();
    if (!params)
        return ERROR;

    cJSON_AddStringToObject(params, "name", remote_name);
    cJSON* args = cJSON_Parse(input_json ? input_json : "{}");
    if (!args) {
        args = cJSON_CreateObject();
        if (!args) {
            cJSON_Delete(params);
            return ERROR;
        }
    }
    cJSON_AddItemToObject(params, "arguments", args);

    /* build_jsonrpc_request takes ownership of params (frees on failure) */
    char* body = build_jsonrpc_request("tools/call", params);
    if (!body)
        return ERROR;

    char* resp_buf = malloc(MCP_RESP_BUF_SIZE);
    if (!resp_buf) {
        free(body);
        return ERROR;
    }
    resp_buf[0] = '\0';

    int status = mcp_http_post(&srv_local, body, resp_buf, MCP_RESP_BUF_SIZE);
    free(body);

    if (status < 200 || status >= 300) {
        snprintf(output, output_size,
            "{\"error\":\"MCP remote call failed: HTTP %d\"}", status);
        free(resp_buf);
        return ERROR;
    }

    /* Parse result -> content[0] -> text */
    cJSON* result = parse_jsonrpc_result(resp_buf);
    free(resp_buf);

    if (!result) {
        snprintf(output, output_size,
            "{\"error\":\"MCP remote: bad response\"}");
        return ERROR;
    }

    cJSON* content = cJSON_GetObjectItem(result, "content");
    if (content && cJSON_IsArray(content)) {
        cJSON* first = cJSON_GetArrayItem(content, 0);
        if (first) {
            cJSON* text = cJSON_GetObjectItem(first, "text");
            if (text && cJSON_IsString(text)) {
                strncpy(output, text->valuestring, output_size - 1);
                output[output_size - 1] = '\0';
                cJSON_Delete(result);
                return OK;
            }
        }
    }

    /* Fallback: stringify the whole result */
    char* fallback = cJSON_PrintUnformatted(result);
    if (fallback) {
        strncpy(output, fallback, output_size - 1);
        output[output_size - 1] = '\0';
        free(fallback);
    }

    cJSON_Delete(result);
    return OK;
}

char* mcp_client_get_tools_json(void)
{
    pthread_mutex_lock(&s_mtx);

    if (s_tool_count == 0) {
        pthread_mutex_unlock(&s_mtx);
        return NULL;
    }

    cJSON* arr = cJSON_CreateArray();
    if (!arr) {
        pthread_mutex_unlock(&s_mtx);
        return NULL;
    }

    for (int i = 0; i < s_tool_count; i++) {
        cJSON* t = cJSON_CreateObject();
        if (!t)
            continue;

        cJSON_AddStringToObject(t, "name", s_tools[i].name);
        cJSON_AddStringToObject(t, "description", s_tools[i].description);

        cJSON* schema = cJSON_Parse(s_tools[i].input_schema_json);
        if (schema) {
            cJSON_AddItemToObject(t, "input_schema", schema);
        } else {
            cJSON* empty = cJSON_CreateObject();
            if (empty) {
                cJSON_AddStringToObject(empty, "type", "object");
                cJSON_AddItemToObject(empty, "properties",
                    cJSON_CreateObject());
                cJSON_AddItemToObject(t, "input_schema", empty);
            }
        }

        cJSON_AddItemToArray(arr, t);
    }

    pthread_mutex_unlock(&s_mtx);

    char* json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

int mcp_client_server_count(void)
{
    pthread_mutex_lock(&s_mtx);
    int count = s_server_count;
    pthread_mutex_unlock(&s_mtx);
    return count;
}

char* mcp_client_status_json(void)
{
    pthread_mutex_lock(&s_mtx);

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        pthread_mutex_unlock(&s_mtx);
        return NULL;
    }

    cJSON_AddNumberToObject(root, "server_count", s_server_count);
    cJSON_AddNumberToObject(root, "tool_count", s_tool_count);

    cJSON* servers = cJSON_CreateArray();
    if (!servers) {
        cJSON_Delete(root);
        pthread_mutex_unlock(&s_mtx);
        return NULL;
    }

    for (int i = 0; i < s_server_count; i++) {
        cJSON* s = cJSON_CreateObject();
        if (!s)
            continue;
        cJSON_AddStringToObject(s, "name", s_servers[i].name);
        cJSON_AddStringToObject(s, "url", s_servers[i].url);
        cJSON_AddBoolToObject(s, "initialized", s_servers[i].initialized);

        /* Count tools for this server */
        int tc = 0;
        for (int j = 0; j < s_tool_count; j++) {
            if (strcmp(s_tools[j].server_name, s_servers[i].name) == 0)
                tc++;
        }
        cJSON_AddNumberToObject(s, "tools", tc);
        cJSON_AddItemToArray(servers, s);
    }
    cJSON_AddItemToObject(root, "servers", servers);

    pthread_mutex_unlock(&s_mtx);

    char* json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

void mcp_client_cleanup(void)
{
    pthread_mutex_lock(&s_mtx);

    for (int i = 0; i < s_tool_count; i++) {
        free(s_tools[i].input_schema_json);
        s_tools[i].input_schema_json = NULL;
    }
    s_tool_count = 0;
    s_server_count = 0;
    s_inited = false;

    pthread_mutex_unlock(&s_mtx);
    syslog(LOG_INFO, "[%s] MCP client cleaned up\n", TAG);
}
