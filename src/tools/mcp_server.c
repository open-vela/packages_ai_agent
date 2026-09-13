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

#include "tools/mcp_server.h"
#include "tools/tool_registry.h"
#include "agent_config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <syslog.h>
#include <sys/socket.h>
#include "cJSON.h"

/* JSON-RPC error codes */
#define JSONRPC_PARSE_ERROR      -32700
#define JSONRPC_INVALID_REQUEST  -32600
#define JSONRPC_METHOD_NOT_FOUND -32601
#define JSONRPC_INVALID_PARAMS   -32602
#define JSONRPC_INTERNAL_ERROR   -32603

static char *create_error_response(const char *id, int code, const char *message)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");

    cJSON *error = cJSON_CreateObject();
    cJSON_AddNumberToObject(error, "code", code);
    cJSON_AddStringToObject(error, "message", message);
    cJSON_AddItemToObject(root, "error", error);

    if (id) cJSON_AddStringToObject(root, "id", id);
    else    cJSON_AddNullToObject(root, "id");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

static char *create_success_response(const char *id, cJSON *result)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "jsonrpc", "2.0");
    cJSON_AddItemToObject(root, "result", result);

    if (id) cJSON_AddStringToObject(root, "id", id);
    else    cJSON_AddNullToObject(root, "id");

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return json;
}

/* ==================== Tools Handlers ==================== */

static char *handle_tools_list(mcp_server_t *server, const char *id)
{
    pthread_rwlock_rdlock(&server->tool_lock);

    cJSON *tools_array = cJSON_CreateArray();
    for (int i = 0; i < server->tool_count; i++) {
        mcp_tool_t *tool = &server->tools[i];
        cJSON *tool_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(tool_obj, "name", tool->name);
        cJSON_AddStringToObject(tool_obj, "description", tool->description);

        if (tool->param_count > 0) {
            cJSON *params = cJSON_CreateObject();
            cJSON *properties = cJSON_CreateObject();
            cJSON *required = cJSON_CreateArray();

            for (int j = 0; j < tool->param_count; j++) {
                cJSON *p = cJSON_CreateObject();
                cJSON_AddStringToObject(p, "type", "string");
                cJSON_AddStringToObject(p, "description", tool->params[j].description);
                cJSON_AddItemToObject(properties, tool->params[j].name, p);
                if (tool->params[j].required)
                    cJSON_AddItemToArray(required, cJSON_CreateString(tool->params[j].name));
            }

            cJSON_AddStringToObject(params, "type", "object");
            cJSON_AddItemToObject(params, "properties", properties);
            if (cJSON_GetArraySize(required) > 0)
                cJSON_AddItemToObject(params, "required", required);
            else
                cJSON_Delete(required);

            cJSON_AddItemToObject(tool_obj, "inputSchema", params);
        }
        cJSON_AddItemToArray(tools_array, tool_obj);
    }

    pthread_rwlock_unlock(&server->tool_lock);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "tools", tools_array);
    return create_success_response(id, result);
}

static char *handle_tools_call(mcp_server_t *server, const char *id, cJSON *params)
{
    cJSON *name_obj = cJSON_GetObjectItem(params, "name");
    cJSON *args_obj = cJSON_GetObjectItem(params, "arguments");

    if (!name_obj || !cJSON_IsString(name_obj))
        return create_error_response(id, JSONRPC_INVALID_PARAMS, "Missing tool name");

    const char *tool_name = name_obj->valuestring;
    char *args_json = args_obj ? cJSON_PrintUnformatted(args_obj) : strdup("{}");

    pthread_rwlock_rdlock(&server->tool_lock);

    mcp_tool_t *tool = NULL;
    for (int i = 0; i < server->tool_count; i++) {
        if (strcmp(server->tools[i].name, tool_name) == 0) {
            tool = &server->tools[i];
            break;
        }
    }

    if (!tool) {
        pthread_rwlock_unlock(&server->tool_lock);
        free(args_json);
        return create_error_response(id, JSONRPC_METHOD_NOT_FOUND, "Tool not found");
    }

    tool->call_count++;
    char *result_str = tool->callback(args_json, tool->user_data);
    pthread_rwlock_unlock(&server->tool_lock);
    free(args_json);

    if (!result_str)
        return create_error_response(id, JSONRPC_INTERNAL_ERROR, "Tool execution failed");

    cJSON *result = cJSON_CreateObject();
    cJSON *content_array = cJSON_CreateArray();
    cJSON *content_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(content_obj, "type", "text");
    cJSON_AddStringToObject(content_obj, "text", result_str);
    cJSON_AddItemToArray(content_array, content_obj);
    cJSON_AddItemToObject(result, "content", content_array);

    free(result_str);
    return create_success_response(id, result);
}

/* ==================== Resources Handlers ==================== */

static char *handle_resources_list(mcp_server_t *server, const char *id)
{
    pthread_rwlock_rdlock(&server->resource_lock);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < server->resource_count; i++) {
        mcp_resource_t *r = &server->resources[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "uri", r->uri);
        cJSON_AddStringToObject(obj, "name", r->name);
        cJSON_AddStringToObject(obj, "description", r->description);
        cJSON_AddStringToObject(obj, "mimeType", r->mime_type);
        cJSON_AddItemToArray(arr, obj);
    }
    pthread_rwlock_unlock(&server->resource_lock);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "resources", arr);
    return create_success_response(id, result);
}

static char *handle_resources_read(mcp_server_t *server, const char *id, cJSON *params)
{
    cJSON *uri_obj = cJSON_GetObjectItem(params, "uri");
    if (!uri_obj || !cJSON_IsString(uri_obj))
        return create_error_response(id, JSONRPC_INVALID_PARAMS, "Missing resource URI");

    const char *uri = uri_obj->valuestring;
    pthread_rwlock_rdlock(&server->resource_lock);

    mcp_resource_t *res = NULL;
    for (int i = 0; i < server->resource_count; i++) {
        if (strcmp(server->resources[i].uri, uri) == 0) { res = &server->resources[i]; break; }
    }
    if (!res) {
        pthread_rwlock_unlock(&server->resource_lock);
        return create_error_response(id, JSONRPC_METHOD_NOT_FOUND, "Resource not found");
    }

    char *content = res->read_callback(uri, res->user_data);
    pthread_rwlock_unlock(&server->resource_lock);

    if (!content)
        return create_error_response(id, JSONRPC_INTERNAL_ERROR, "Failed to read resource");

    cJSON *result = cJSON_CreateObject();
    cJSON *contents = cJSON_CreateArray();
    cJSON *c = cJSON_CreateObject();
    cJSON_AddStringToObject(c, "uri", uri);
    cJSON_AddStringToObject(c, "mimeType", res->mime_type);
    cJSON_AddStringToObject(c, "text", content);
    cJSON_AddItemToArray(contents, c);
    cJSON_AddItemToObject(result, "contents", contents);
    free(content);
    return create_success_response(id, result);
}

/* ==================== Prompts Handlers ==================== */

static char *handle_prompts_list(mcp_server_t *server, const char *id)
{
    pthread_rwlock_rdlock(&server->prompt_lock);
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < server->prompt_count; i++) {
        mcp_prompt_t *p = &server->prompts[i];
        cJSON *obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", p->name);
        cJSON_AddStringToObject(obj, "description", p->description);
        if (p->arg_count > 0) {
            cJSON *args = cJSON_CreateArray();
            for (int j = 0; j < p->arg_count; j++) {
                cJSON *a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "name", p->args[j].name);
                cJSON_AddStringToObject(a, "description", p->args[j].description);
                cJSON_AddBoolToObject(a, "required", p->args[j].required);
                cJSON_AddItemToArray(args, a);
            }
            cJSON_AddItemToObject(obj, "arguments", args);
        }
        cJSON_AddItemToArray(arr, obj);
    }
    pthread_rwlock_unlock(&server->prompt_lock);

    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "prompts", arr);
    return create_success_response(id, result);
}

static char *handle_prompts_get(mcp_server_t *server, const char *id, cJSON *params)
{
    cJSON *name_obj = cJSON_GetObjectItem(params, "name");
    if (!name_obj || !cJSON_IsString(name_obj))
        return create_error_response(id, JSONRPC_INVALID_PARAMS, "Missing prompt name");

    const char *prompt_name = name_obj->valuestring;
    cJSON *args_obj = cJSON_GetObjectItem(params, "arguments");
    char *args_json = args_obj ? cJSON_PrintUnformatted(args_obj) : NULL;

    pthread_rwlock_rdlock(&server->prompt_lock);
    mcp_prompt_t *prompt = NULL;
    for (int i = 0; i < server->prompt_count; i++) {
        if (strcmp(server->prompts[i].name, prompt_name) == 0) { prompt = &server->prompts[i]; break; }
    }
    if (!prompt) {
        pthread_rwlock_unlock(&server->prompt_lock);
        free(args_json);
        return create_error_response(id, JSONRPC_METHOD_NOT_FOUND, "Prompt not found");
    }

    int msg_count = 0;
    mcp_prompt_message_t *msgs = prompt->callback(prompt_name, args_json, &msg_count, prompt->user_data);
    pthread_rwlock_unlock(&server->prompt_lock);
    free(args_json);

    if (!msgs || msg_count == 0)
        return create_error_response(id, JSONRPC_INTERNAL_ERROR, "Failed to generate prompt");

    cJSON *result = cJSON_CreateObject();
    cJSON *messages = cJSON_CreateArray();
    for (int i = 0; i < msg_count; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "role", msgs[i].role);
        cJSON *co = cJSON_CreateObject();
        cJSON_AddStringToObject(co, "type", "text");
        cJSON_AddStringToObject(co, "text", msgs[i].content);
        cJSON_AddItemToObject(m, "content", co);
        cJSON_AddItemToArray(messages, m);
        free(msgs[i].content);
    }
    free(msgs);
    cJSON_AddItemToObject(result, "messages", messages);
    return create_success_response(id, result);
}

/* ==================== Public Functions ==================== */

int mcp_server_init(mcp_server_t *server, const char *name, const char *version)
{
    if (!server || !name || !version) return -1;
    memset(server, 0, sizeof(*server));
    strncpy(server->name, name, sizeof(server->name) - 1);
    strncpy(server->version, version, sizeof(server->version) - 1);
    pthread_rwlock_init(&server->tool_lock, NULL);
    pthread_rwlock_init(&server->resource_lock, NULL);
    pthread_rwlock_init(&server->prompt_lock, NULL);
    server->initialized = true;
    return 0;
}

void mcp_server_destroy(mcp_server_t *server)
{
    if (!server || !server->initialized) return;
    if (server->stdio_running) mcp_server_stop_stdio(server);
    pthread_rwlock_destroy(&server->tool_lock);
    pthread_rwlock_destroy(&server->resource_lock);
    pthread_rwlock_destroy(&server->prompt_lock);
    server->initialized = false;
}

int mcp_server_register_tool(mcp_server_t *server,
                              const char *name, const char *description,
                              const mcp_param_t *params, int param_count,
                              mcp_tool_fn callback, void *user_data,
                              bool is_streaming)
{
    if (!server || !name || !callback || param_count > MCP_MAX_PARAMS) return -1;
    pthread_rwlock_wrlock(&server->tool_lock);
    if (server->tool_count >= MCP_MAX_TOOLS) {
        pthread_rwlock_unlock(&server->tool_lock);
        return -1;
    }
    mcp_tool_t *t = &server->tools[server->tool_count];
    strncpy(t->name, name, sizeof(t->name) - 1);
    strncpy(t->description, description ? description : "", sizeof(t->description) - 1);
    t->param_count = param_count;
    if (params && param_count > 0) memcpy(t->params, params, param_count * sizeof(mcp_param_t));
    t->callback = callback;
    t->user_data = user_data;
    t->is_streaming = is_streaming;
    t->call_count = 0;
    server->tool_count++;
    pthread_rwlock_unlock(&server->tool_lock);
    return 0;
}

int mcp_server_register_resource(mcp_server_t *server,
                                  const char *uri, const char *name,
                                  const char *description, const char *mime_type,
                                  mcp_resource_fn callback, void *user_data)
{
    if (!server || !uri || !name || !callback) return -1;
    pthread_rwlock_wrlock(&server->resource_lock);
    if (server->resource_count >= MCP_MAX_RESOURCES) {
        pthread_rwlock_unlock(&server->resource_lock);
        return -1;
    }
    mcp_resource_t *r = &server->resources[server->resource_count];
    strncpy(r->uri, uri, sizeof(r->uri) - 1);
    strncpy(r->name, name, sizeof(r->name) - 1);
    strncpy(r->description, description ? description : "", sizeof(r->description) - 1);
    strncpy(r->mime_type, mime_type ? mime_type : "text/plain", sizeof(r->mime_type) - 1);
    r->read_callback = callback;
    r->user_data = user_data;
    server->resource_count++;
    pthread_rwlock_unlock(&server->resource_lock);
    return 0;
}

int mcp_server_register_prompt(mcp_server_t *server,
                                const char *name, const char *description,
                                const mcp_param_t *args, int arg_count,
                                mcp_prompt_fn callback, void *user_data)
{
    if (!server || !name || !callback || arg_count > MCP_MAX_PARAMS) return -1;
    pthread_rwlock_wrlock(&server->prompt_lock);
    if (server->prompt_count >= MCP_MAX_PROMPTS) {
        pthread_rwlock_unlock(&server->prompt_lock);
        return -1;
    }
    mcp_prompt_t *p = &server->prompts[server->prompt_count];
    strncpy(p->name, name, sizeof(p->name) - 1);
    strncpy(p->description, description ? description : "", sizeof(p->description) - 1);
    p->arg_count = arg_count;
    if (args && arg_count > 0) memcpy(p->args, args, arg_count * sizeof(mcp_param_t));
    p->callback = callback;
    p->user_data = user_data;
    server->prompt_count++;
    pthread_rwlock_unlock(&server->prompt_lock);
    return 0;
}

char *mcp_server_handle_request(mcp_server_t *server, const char *request_json)
{
    if (!server || !request_json)
        return create_error_response(NULL, JSONRPC_INVALID_REQUEST, "Invalid request");

    cJSON *root = cJSON_Parse(request_json);
    if (!root) return create_error_response(NULL, JSONRPC_PARSE_ERROR, "Parse error");

    cJSON *method = cJSON_GetObjectItem(root, "method");
    cJSON *params = cJSON_GetObjectItem(root, "params");
    cJSON *id = cJSON_GetObjectItem(root, "id");

    const char *id_str = NULL;
    static char id_buf[32];
    if (id && cJSON_IsString(id))       id_str = id->valuestring;
    else if (id && cJSON_IsNumber(id)) { snprintf(id_buf, sizeof(id_buf), "%lld", (long long)id->valueint); id_str = id_buf; }

    if (!method || !cJSON_IsString(method)) {
        cJSON_Delete(root);
        return create_error_response(id_str, JSONRPC_INVALID_REQUEST, "Invalid request");
    }

    const char *m = method->valuestring;
    char *response = NULL;

    if      (strcmp(m, "tools/list") == 0)      response = handle_tools_list(server, id_str);
    else if (strcmp(m, "tools/call") == 0)       response = handle_tools_call(server, id_str, params);
    else if (strcmp(m, "resources/list") == 0)   response = handle_resources_list(server, id_str);
    else if (strcmp(m, "resources/read") == 0)   response = handle_resources_read(server, id_str, params);
    else if (strcmp(m, "prompts/list") == 0)     response = handle_prompts_list(server, id_str);
    else if (strcmp(m, "prompts/get") == 0)      response = handle_prompts_get(server, id_str, params);
    else response = create_error_response(id_str, JSONRPC_METHOD_NOT_FOUND, "Method not found");

    cJSON_Delete(root);
    return response;
}

/* ==================== stdio Transport ==================== */

static void *stdio_thread(void *arg)
{
    mcp_server_t *server = (mcp_server_t *)arg;
    char *line = NULL;
    size_t len = 0;
    ssize_t nread;

    while (server->stdio_running) {
        nread = getline(&line, &len, stdin);
        if (nread <= 0) break;
        if (line[nread - 1] == '\n') line[nread - 1] = '\0';

        char *resp = mcp_server_handle_request(server, line);
        if (resp) { fprintf(stdout, "%s\n", resp); fflush(stdout); free(resp); }
    }
    free(line);
    return NULL;
}

int mcp_server_start_stdio(mcp_server_t *server)
{
    if (!server || server->stdio_running) return -1;
    server->stdio_running = true;
    if (pthread_create(&server->stdio_thread, NULL, stdio_thread, server) != 0) {
        server->stdio_running = false;
        return -1;
    }
    return 0;
}

void mcp_server_stop_stdio(mcp_server_t *server)
{
    if (!server || !server->stdio_running) return;
    server->stdio_running = false;
    pthread_cancel(server->stdio_thread);
    pthread_join(server->stdio_thread, NULL);
}

/* ══════════════════════════════════════════════════════════════
 *  HTTP Transport — for remote MCP client integration via ws_server
 *
 *  Unlike stdio transport which uses mcp_server's own tool registry,
 *  HTTP transport bridges to the main tool_registry (36+ real tools).
 *  This lets remote clients discover and call all device tools via standard MCP.
 *
 *  Remote client config:
 *    {"name":"vela-watch","url":"http://<IP>:28789/mcp","transport":"http"}
 * ══════════════════════════════════════════════════════════════ */

static const char *HTTP_TAG = "mcp_srv";

static void http_respond(int fd, int status, const char *status_text,
                         const char *body)
{
    int body_len = body ? (int)strlen(body) : 0;
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        status, status_text, body_len);
    if (hlen >= (int)sizeof(hdr))
        hlen = (int)sizeof(hdr) - 1;
    send(fd, hdr, hlen, 0);
    if (body && body_len > 0)
        send(fd, body, body_len, 0);
}

static void http_jsonrpc_result(int fd, cJSON *id, cJSON *result)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) { cJSON_Delete(result); return; }
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", id ? cJSON_Duplicate(id, 1)
                                         : cJSON_CreateNull());
    cJSON_AddItemToObject(resp, "result", result);
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (json) { http_respond(fd, 200, "OK", json); free(json); }
}

static void http_jsonrpc_error(int fd, cJSON *id, int code, const char *msg)
{
    cJSON *resp = cJSON_CreateObject();
    if (!resp) return;
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddItemToObject(resp, "id", id ? cJSON_Duplicate(id, 1)
                                         : cJSON_CreateNull());
    cJSON *err = cJSON_AddObjectToObject(resp, "error");
    if (err) {
        cJSON_AddNumberToObject(err, "code", code);
        cJSON_AddStringToObject(err, "message", msg);
    }
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    if (json) { http_respond(fd, 200, "OK", json); free(json); }
}

static void http_handle_initialize(int fd, cJSON *id)
{
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "protocolVersion", "2024-11-05");
    cJSON *caps = cJSON_AddObjectToObject(r, "capabilities");
    if (caps) cJSON_AddObjectToObject(caps, "tools");
    cJSON *info = cJSON_AddObjectToObject(r, "serverInfo");
    if (info) {
        cJSON_AddStringToObject(info, "name",
            CONFIG_EXAMPLES_AI_AGENT_VELA_PROGNAME);
        cJSON_AddStringToObject(info, "version", "1.0.0");
    }
    syslog(LOG_INFO, "[%s] initialize\n", HTTP_TAG);
    http_jsonrpc_result(fd, id, r);
}

static void http_handle_tools_list(int fd, cJSON *id)
{
    char *src_json = tool_registry_get_tools_json();
    cJSON *r = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();

    if (src_json) {
        cJSON *src = cJSON_Parse(src_json);
        if (src) {
            cJSON *t;
            cJSON_ArrayForEach(t, src) {
                cJSON *n = cJSON_GetObjectItem(t, "name");
                cJSON *d = cJSON_GetObjectItem(t, "description");
                cJSON *s = cJSON_GetObjectItem(t, "input_schema");
                cJSON *tool = cJSON_CreateObject();
                if (!tool) continue;
                if (cJSON_IsString(n))
                    cJSON_AddStringToObject(tool, "name", n->valuestring);
                if (cJSON_IsString(d))
                    cJSON_AddStringToObject(tool, "description", d->valuestring);
                cJSON_AddItemToObject(tool, "inputSchema",
                    s ? cJSON_Duplicate(s, 1)
                      : cJSON_Parse("{\"type\":\"object\",\"properties\":{}}"));
                cJSON_AddItemToArray(arr, tool);
            }
            cJSON_Delete(src);
        }
        free(src_json);
    }

    cJSON_AddItemToObject(r, "tools", arr);
    syslog(LOG_INFO, "[%s] tools/list: %d\n", HTTP_TAG, cJSON_GetArraySize(arr));
    http_jsonrpc_result(fd, id, r);
}

static void http_handle_tools_call(int fd, cJSON *id, cJSON *params)
{
    cJSON *name = cJSON_GetObjectItem(params, "name");
    cJSON *args = cJSON_GetObjectItem(params, "arguments");
    if (!cJSON_IsString(name)) {
        http_jsonrpc_error(fd, id, JSONRPC_INVALID_PARAMS, "missing params.name");
        return;
    }

    char *args_str = args ? cJSON_PrintUnformatted(args) : strdup("{}");
    if (!args_str) { http_jsonrpc_error(fd, id, JSONRPC_INTERNAL_ERROR, "OOM"); return; }

    char *output = malloc(8192);
    if (!output) { free(args_str); http_jsonrpc_error(fd, id, JSONRPC_INTERNAL_ERROR, "OOM"); return; }
    output[0] = '\0';

    syslog(LOG_INFO, "[%s] tools/call: %s\n", HTTP_TAG, name->valuestring);
    int rc = tool_registry_execute(name->valuestring, args_str, output, 8192);
    free(args_str);

    cJSON *r = cJSON_CreateObject();
    cJSON *content = cJSON_AddArrayToObject(r, "content");
    if (content) {
        cJSON *item = cJSON_CreateObject();
        if (item) {
            cJSON_AddStringToObject(item, "type", "text");
            cJSON_AddStringToObject(item, "text",
                (rc == 0 && output[0]) ? output : "tool execution failed");
            cJSON_AddItemToArray(content, item);
        }
    }
    if (rc != 0) cJSON_AddBoolToObject(r, "isError", 1);
    free(output);
    http_jsonrpc_result(fd, id, r);
}

static void http_dispatch(int fd, const char *body)
{
    cJSON *req = cJSON_Parse(body);
    if (!req) { http_jsonrpc_error(fd, NULL, JSONRPC_PARSE_ERROR, "parse error"); return; }

    cJSON *method = cJSON_GetObjectItem(req, "method");
    cJSON *id = cJSON_GetObjectItem(req, "id");
    cJSON *params = cJSON_GetObjectItem(req, "params");

    if (!cJSON_IsString(method)) {
        http_jsonrpc_error(fd, id, JSONRPC_INVALID_REQUEST, "missing method");
        cJSON_Delete(req);
        return;
    }

    const char *m = method->valuestring;
    if (strcmp(m, "initialize") == 0)
        http_handle_initialize(fd, id);
    else if (strcmp(m, "notifications/initialized") == 0)
        http_respond(fd, 200, "OK", "");
    else if (strcmp(m, "tools/list") == 0)
        http_handle_tools_list(fd, id);
    else if (strcmp(m, "tools/call") == 0)
        http_handle_tools_call(fd, id, params);
    else
        http_jsonrpc_error(fd, id, JSONRPC_METHOD_NOT_FOUND, "method not found");

    cJSON_Delete(req);
}

bool mcp_server_try_handle_http(int fd, const char *buf, int buf_len)
{
    if (strncmp(buf, "POST ", 5) != 0) return false;
    const char *pe = strchr(buf + 5, ' ');
    if (!pe || (pe - buf - 5) != 4 || strncmp(buf + 5, "/mcp", 4) != 0)
        return false;

    const char *bs = strstr(buf, "\r\n\r\n");
    if (!bs) { http_respond(fd, 400, "Bad Request", ""); return true; }
    bs += 4;

    int bib = buf_len - (int)(bs - buf);
    char body[4096];
    if (bib > 0 && bib < (int)sizeof(body))
        memcpy(body, bs, bib);
    else
        bib = 0;

    const char *cl = strcasestr(buf, "\r\nContent-Length: ");
    int clen = cl ? atoi(cl + 18) : 0;
    if (clen > (int)sizeof(body) - 1) clen = (int)sizeof(body) - 1;
    while (bib < clen) {
        int n = recv(fd, body + bib, clen - bib, 0);
        if (n <= 0) break;
        bib += n;
    }
    body[bib] = '\0';

    http_dispatch(fd, body);
    return true;
}
