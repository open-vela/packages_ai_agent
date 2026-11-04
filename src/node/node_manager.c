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

#include "node/node_manager.h"
#include "core/message_bus.h"
#include "cJSON.h"
#include "tools/tool_registry.h"
#include "agent_compat.h"
#include "agent_config.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "node_mgr";

/* ── Node table ────────────────────────────────────────────── */

typedef struct {
    bool active;
    int ws_fd;
    char node_id[NODE_ID_LEN];
    char display_name[64];
    char platform[16];
    char device_family[16];
    char commands[NODE_MAX_COMMANDS][NODE_CMD_LEN];
    int cmd_count;
} node_entry_t;

static node_entry_t s_nodes[NODE_MAX_NODES];
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ── Pending invoke table (synchronous dispatch) ───────────── */

#define MAX_PENDING 4
#define INVOKE_TIMEOUT_S 30

typedef struct {
    bool active;
    char invoke_id[48];
    bool done;
    bool ok;
    char* result_json; /* heap, caller frees */
    pthread_mutex_t mtx;
    pthread_cond_t cond;
} pending_invoke_t;

static pending_invoke_t s_pending[MAX_PENDING];

/* ── WS frame write lock ──────────────────────────────────── */
/* Protects ws_send_text_to_fd() from concurrent callers.
 * Without this, parallel tool threads invoking the same node
 * interleave header + payload bytes on the fd, corrupting the
 * WebSocket frame stream and causing the remote side to see
 * garbled data (observed as "JSON parse failed" + disconnect). */

static pthread_mutex_t s_ws_write_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ── WS frame send (server→client, no mask) ────────────────── */

static int ws_send_text_to_fd(int fd, const char* data, size_t len)
{
    unsigned char hdr[10];
    int hdr_len = 0;

    hdr[0] = 0x81; /* FIN + text */
    if (len < 126) {
        hdr[1] = (unsigned char)len;
        hdr_len = 2;
    } else if (len < 65536) {
        hdr[1] = 126;
        hdr[2] = (unsigned char)(len >> 8);
        hdr[3] = (unsigned char)(len & 0xFF);
        hdr_len = 4;
    } else {
        return -1;
    }

    pthread_mutex_lock(&s_ws_write_mtx);
    int ret = 0;

    if (send(fd, hdr, hdr_len, 0) != hdr_len) {
        ret = -1;
    } else if (send(fd, data, len, 0) != (int)len) {
        ret = -1;
    }

    pthread_mutex_unlock(&s_ws_write_mtx);
    return ret;
}

/* ── ID generator ──────────────────────────────────────────── */

static void gen_id(char* buf, size_t cap)
{
    unsigned char rnd[4];
    if (agent_secure_random(rnd, sizeof(rnd)) == 0) {
        snprintf(buf, cap, "%02x%02x%02x%02x-%04x",
            rnd[0], rnd[1], rnd[2], rnd[3],
            (unsigned)((rnd[0] << 8) | rnd[1]) & 0xFFFF);
    } else {
        /* Fallback: use address + time as unique-ish ID (not for crypto) */
        snprintf(buf, cap, "%08lx-%04x", (unsigned long)time(NULL),
            (unsigned)((uintptr_t)buf & 0xFFFF));
    }
}

/* ── Send JSON to a node fd ────────────────────────────────── */

static int send_event(int fd, const char* event, cJSON* payload)
{
    cJSON* frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "evt");
    cJSON_AddStringToObject(frame, "event", event);
    if (payload)
        cJSON_AddItemToObject(frame, "payload", payload);

    char* json = cJSON_PrintUnformatted(frame);
    int ret = ws_send_text_to_fd(fd, json, strlen(json));
    free(json);
    cJSON_Delete(frame);
    return ret;
}

static int send_response(int fd, const char* req_id, bool ok, cJSON* payload,
    cJSON* error)
{
    cJSON* frame = cJSON_CreateObject();
    cJSON_AddStringToObject(frame, "type", "res");
    cJSON_AddStringToObject(frame, "id", req_id);
    cJSON_AddBoolToObject(frame, "ok", ok);
    if (ok && payload)
        cJSON_AddItemToObject(frame, "payload", payload);
    else if (!ok && error)
        cJSON_AddItemToObject(frame, "error", error);

    char* json = cJSON_PrintUnformatted(frame);
    int ret = ws_send_text_to_fd(fd, json, strlen(json));
    free(json);
    cJSON_Delete(frame);
    return ret;
}

/* ── Sanitize string to OpenAI tool-name safe chars [a-zA-Z0-9_.-] ── */

static void sanitize_tool_name_part(char* s)
{
    bool changed = false;
    for (char* p = s; *p; p++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
                || (*p >= '0' && *p <= '9') || *p == '_' || *p == '-')) {
            if (!changed)
                syslog(LOG_WARNING, "[%s] Sanitizing tool name part: '%s' "
                                    "(illegal char '%c' at pos %d)\n",
                    TAG, s, *p, (int)(p - s));
            *p = '_';
            changed = true;
        }
    }
    if (changed)
        syslog(LOG_INFO, "[%s] Sanitized result: '%s'\n", TAG, s);
}

/* ── Node lookup helpers ───────────────────────────────────── */

static node_entry_t* find_node_by_fd(int fd)
{
    for (int i = 0; i < NODE_MAX_NODES; i++) {
        if (s_nodes[i].active && s_nodes[i].ws_fd == fd)
            return &s_nodes[i];
    }
    return NULL;
}

static node_entry_t* find_node_by_id(const char* id)
{
    for (int i = 0; i < NODE_MAX_NODES; i++) {
        if (s_nodes[i].active && strcmp(s_nodes[i].node_id, id) == 0)
            return &s_nodes[i];
    }
    return NULL;
}

/* ── Pending invoke helpers ────────────────────────────────── */

static pending_invoke_t* alloc_pending(const char* invoke_id)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!s_pending[i].active) {
            pending_invoke_t* p = &s_pending[i];
            memset(p, 0, sizeof(*p));
            p->active = true;
            strncpy(p->invoke_id, invoke_id, sizeof(p->invoke_id) - 1);
            pthread_mutex_init(&p->mtx, NULL);
            pthread_cond_init(&p->cond, NULL);
            return p;
        }
    }
    return NULL;
}

static void free_pending(pending_invoke_t* p)
{
    p->active = false;
    pthread_mutex_destroy(&p->mtx);
    pthread_cond_destroy(&p->cond);
}

static pending_invoke_t* find_pending(const char* invoke_id)
{
    for (int i = 0; i < MAX_PENDING; i++) {
        if (s_pending[i].active && strcmp(s_pending[i].invoke_id, invoke_id) == 0)
            return &s_pending[i];
    }
    return NULL;
}

/* ── Handle connect request from a Node ────────────────────── */

static void handle_connect(int ws_fd, const char* req_id, cJSON* params)
{
    cJSON* client = cJSON_GetObjectItem(params, "client");
    const char* node_id = cJSON_GetStringValue(cJSON_GetObjectItem(client, "id"));
    const char* display = cJSON_GetStringValue(cJSON_GetObjectItem(client, "displayName"));
    const char* platform = cJSON_GetStringValue(cJSON_GetObjectItem(client, "platform"));
    const char* family = cJSON_GetStringValue(cJSON_GetObjectItem(client, "deviceFamily"));

    if (!node_id || !node_id[0]) {
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "code", "INVALID_CLIENT");
        cJSON_AddStringToObject(err, "message", "missing client.id");
        send_response(ws_fd, req_id, false, NULL, err);
        return;
    }

    /* Sanitize node_id early so reconnect lookup matches stored id */
    char safe_id[NODE_ID_LEN];
    strncpy(safe_id, node_id, NODE_ID_LEN - 1);
    safe_id[NODE_ID_LEN - 1] = '\0';
    sanitize_tool_name_part(safe_id);

    pthread_mutex_lock(&s_mtx);

    /* Check for duplicate — replace if same id reconnects */
    node_entry_t* existing = find_node_by_id(safe_id);
    if (existing) {
        syslog(LOG_INFO, "[%s] Node %s reconnected (old fd=%d, new fd=%d)\n", TAG,
            node_id, existing->ws_fd, ws_fd);
        existing->ws_fd = ws_fd;
    }

    node_entry_t* node = existing;
    if (!node) {
        /* Find free slot */
        for (int i = 0; i < NODE_MAX_NODES; i++) {
            if (!s_nodes[i].active) {
                node = &s_nodes[i];
                break;
            }
        }
    }

    if (!node) {
        pthread_mutex_unlock(&s_mtx);
        cJSON* err = cJSON_CreateObject();
        cJSON_AddStringToObject(err, "code", "MAX_NODES");
        cJSON_AddStringToObject(err, "message", "node limit reached");
        send_response(ws_fd, req_id, false, NULL, err);
        return;
    }

    memset(node, 0, sizeof(*node));
    node->active = true;
    node->ws_fd = ws_fd;
    strncpy(node->node_id, safe_id, NODE_ID_LEN - 1);
    if (display)
        strncpy(node->display_name, display, sizeof(node->display_name) - 1);
    if (platform)
        strncpy(node->platform, platform, sizeof(node->platform) - 1);
    if (family)
        strncpy(node->device_family, family, sizeof(node->device_family) - 1);

    /* Parse commands */
    cJSON* cmds = cJSON_GetObjectItem(params, "commands");
    if (cmds && cJSON_IsArray(cmds)) {
        cJSON* c = NULL;
        cJSON_ArrayForEach(c, cmds)
        {
            if (node->cmd_count >= NODE_MAX_COMMANDS)
                break;
            if (cJSON_IsString(c)) {
                strncpy(node->commands[node->cmd_count], c->valuestring,
                    NODE_CMD_LEN - 1);
                sanitize_tool_name_part(node->commands[node->cmd_count]);
                node->cmd_count++;
            }
        }
    }

    pthread_mutex_unlock(&s_mtx);

    syslog(LOG_INFO, "[%s] Node connected: %s (%s/%s) with %d commands\n", TAG,
        node_id, platform ? platform : "?", family ? family : "?",
        node->cmd_count);

    /* Node tools changed — mark registry dirty for rebuild */
    tool_registry_invalidate();

    /* Send hello-ok */
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "type", "hello-ok");
    cJSON* server = cJSON_CreateObject();
    char conn_id[32];
    gen_id(conn_id, sizeof(conn_id));
    cJSON_AddStringToObject(server, "connId", conn_id);
    cJSON_AddItemToObject(payload, "server", server);
    send_response(ws_fd, req_id, true, payload, NULL);

    /* Rebuild tools JSON so the LLM sees the new node commands */
    tool_registry_rebuild_json();
}

/* ── Handle node.invoke.result from a Node ─────────────────── */

static void handle_invoke_result(cJSON* params)
{
    const char* invoke_id = cJSON_GetStringValue(cJSON_GetObjectItem(params, "id"));
    if (!invoke_id)
        return;

    pending_invoke_t* p = find_pending(invoke_id);
    if (!p) {
        syslog(LOG_WARNING, "[%s] No pending invoke for id=%s\n", TAG, invoke_id);
        return;
    }

    pthread_mutex_lock(&p->mtx);
    p->ok = cJSON_IsTrue(cJSON_GetObjectItem(params, "ok"));

    if (p->ok) {
        const char* pj = cJSON_GetStringValue(cJSON_GetObjectItem(params, "payloadJSON"));
        if (pj)
            p->result_json = strdup(pj);
    } else {
        cJSON* err = cJSON_GetObjectItem(params, "error");
        const char* msg = cJSON_GetStringValue(cJSON_GetObjectItem(err, "message"));
        p->result_json = strdup(msg ? msg : "node error");
    }

    p->done = true;
    pthread_cond_signal(&p->cond);
    pthread_mutex_unlock(&p->mtx);
}

/* ── Public API ────────────────────────────────────────────── */

int node_manager_init(void)
{
    memset(s_nodes, 0, sizeof(s_nodes));
    memset(s_pending, 0, sizeof(s_pending));

    /* Register as tool provider to break circular dependency */
    tool_registry_register_provider("node",
                                    node_manager_get_tools_json,
                                    node_manager_execute);

    syslog(LOG_INFO, "[%s] Node manager initialized (max %d nodes)\n", TAG,
        NODE_MAX_NODES);
    return OK;
}

bool node_manager_handle_message(int ws_fd, const char* data, int len)
{
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (!root) {
        syslog(LOG_DEBUG, "[%s] handle_message: JSON parse failed fd=%d len=%d\n",
            TAG, ws_fd, len);
        return false;
    }

    const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(root, "type"));
    if (!type) {
        cJSON_Delete(root);
        return false;
    }

    /* ── Request from Node ─────────────────────────────────── */
    if (strcmp(type, "req") == 0) {
        const char* method = cJSON_GetStringValue(cJSON_GetObjectItem(root, "method"));
        const char* req_id = cJSON_GetStringValue(cJSON_GetObjectItem(root, "id"));
        cJSON* params = cJSON_GetObjectItem(root, "params");

        if (!method || !req_id) {
            cJSON_Delete(root);
            return false;
        }

        if (strcmp(method, "connect") == 0) {
            /* First, check if this is a Node connect (role=node) */
            const char* role = cJSON_GetStringValue(cJSON_GetObjectItem(params, "role"));
            if (!role || strcmp(role, "node") != 0) {
                cJSON_Delete(root);
                return false; /* Not a node connect — let ws_server handle it */
            }
            handle_connect(ws_fd, req_id, params);
            cJSON_Delete(root);
            return true;
        }

        if (strcmp(method, "node.invoke.result") == 0) {
            if (params)
                handle_invoke_result(params);
            cJSON_Delete(root);
            return true;
        }

        /* Node asking gateway to forward a chat message (e.g. cron reminder
         * fired on a node that has no local feishu credentials). */
        if (strcmp(method, "chat.forward") == 0) {
            cJSON* pl = params ? cJSON_GetObjectItem(params, "payload") : NULL;
            if (!pl)
                pl = params; /* accept flat params too */
            if (pl) {
                const char* ch = cJSON_GetStringValue(cJSON_GetObjectItem(pl, "channel"));
                const char* cid = cJSON_GetStringValue(cJSON_GetObjectItem(pl, "chat_id"));
                const char* ct = cJSON_GetStringValue(cJSON_GetObjectItem(pl, "content"));
                if (ch && cid && ct) {
                    syslog(LOG_INFO, "[%s] chat.forward from node: %s:%s\n", TAG, ch, cid);
                    /* Push to outbound bus so the local dispatcher delivers it */
                    agent_msg_t msg;
                    memset(&msg, 0, sizeof(msg));
                    strncpy(msg.channel, ch, sizeof(msg.channel) - 1);
                    strncpy(msg.chat_id, cid, sizeof(msg.chat_id) - 1);
                    /* 转发的是别的节点产出的回答，按最终回答处理。 */
                    msg.kind = AGENT_MSG_KIND_FINAL;
                    msg.content = strdup(ct);
                    if (msg.content) {
                        if (message_bus_push_outbound(&msg) != OK)
                            free(msg.content);
                    }
                }
            }
            /* Send ack */
            cJSON* ack = cJSON_CreateObject();
            cJSON_AddStringToObject(ack, "type", "ok");
            send_response(ws_fd, req_id, true, ack, NULL);
            cJSON_Delete(root);
            return true;
        }

        cJSON_Delete(root);
        return false;
    }

    /* ── Response from Node (e.g. invoke result) ───────────── */
    if (strcmp(type, "res") == 0) {
        /* Check if this is a response to one of our invoke requests */
        cJSON* payload = cJSON_GetObjectItem(root, "payload");
        if (payload) {
            const char* id = cJSON_GetStringValue(cJSON_GetObjectItem(payload, "id"));
            if (id && find_pending(id)) {
                handle_invoke_result(payload);
                cJSON_Delete(root);
                return true;
            }
        }
        cJSON_Delete(root);
        return false;
    }

    cJSON_Delete(root);
    return false;
}

void node_manager_on_disconnect(int ws_fd)
{
    pthread_mutex_lock(&s_mtx);
    node_entry_t* node = find_node_by_fd(ws_fd);
    if (node) {
        syslog(LOG_INFO, "[%s] Node disconnected: %s (%d commands removed)\n", TAG,
            node->node_id, node->cmd_count);
        node->active = false;
    }
    pthread_mutex_unlock(&s_mtx);

    /* Node tools changed — mark registry dirty for rebuild */
    if (node) {
        tool_registry_invalidate();
    }

    /* Wake up any pending invokes so they fail immediately
     * instead of waiting for the full 30s timeout. */
    for (int i = 0; i < MAX_PENDING; i++) {
        if (!s_pending[i].active) {
            continue;
        }
        pthread_mutex_lock(&s_pending[i].mtx);
        if (!s_pending[i].done) {
            s_pending[i].done = true;
            s_pending[i].ok = false;
            s_pending[i].result_json = strdup("node disconnected");
            pthread_cond_signal(&s_pending[i].cond);
        }
        pthread_mutex_unlock(&s_pending[i].mtx);
    }
}

int node_manager_execute(const char* tool_name, const char* input_json,
    char* output, size_t output_size)
{
    /* Tool name format: "node.<node_id>.<command>" */
    if (strncmp(tool_name, "node.", 5) != 0)
        return ERROR;

    const char* rest = tool_name + 5;
    const char* sep = strrchr(rest, '.');
    if (!sep) {
        snprintf(output, output_size, "Invalid node tool format: %s", tool_name);
        return ERROR;
    }

    char node_id[NODE_ID_LEN];
    size_t id_len = (size_t)(sep - rest);
    if (id_len >= NODE_ID_LEN)
        id_len = NODE_ID_LEN - 1;
    memcpy(node_id, rest, id_len);
    node_id[id_len] = '\0';

    const char* command = sep + 1;

    pthread_mutex_lock(&s_mtx);
    node_entry_t* node = find_node_by_id(node_id);
    if (!node) {
        pthread_mutex_unlock(&s_mtx);
        snprintf(output, output_size, "Node '%s' not connected", node_id);
        return ERROR;
    }
    int ws_fd = node->ws_fd;
    pthread_mutex_unlock(&s_mtx);

    /* Create invoke request */
    char invoke_id[48];
    gen_id(invoke_id, sizeof(invoke_id));

    pending_invoke_t* p = alloc_pending(invoke_id);
    if (!p) {
        snprintf(output, output_size, "Too many pending invocations");
        return ERROR;
    }

    /* Send node.invoke.request as event */
    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "id", invoke_id);
    cJSON_AddStringToObject(payload, "nodeId", node_id);
    cJSON_AddStringToObject(payload, "command", command);

    /* Normalize paramsJSON: LLM sometimes sends numeric values as strings
     * (e.g. "at_epoch": "1775654400" instead of "at_epoch": 1775654400).
     * Scan all string values and convert those that look like numbers. */
    const char* params_str = input_json ? input_json : "{}";
    cJSON* params_obj = cJSON_Parse(params_str);
    if (params_obj && cJSON_IsObject(params_obj)) {
        /* Collect keys that need conversion (avoid iterator invalidation) */
        int count = cJSON_GetArraySize(params_obj);
        char** keys = NULL;
        int nkeys = 0;
        if (count > 0) {
            keys = malloc(sizeof(char*) * count);
            cJSON* item = NULL;
            cJSON_ArrayForEach(item, params_obj)
            {
                if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
                    char* endp = NULL;
                    double dval = strtod(item->valuestring, &endp);
                    if (endp && *endp == '\0' && endp != item->valuestring) {
                        keys[nkeys++] = strdup(item->string);
                        (void)dval;
                    }
                }
            }
            for (int i = 0; i < nkeys; i++) {
                cJSON* old = cJSON_GetObjectItem(params_obj, keys[i]);
                if (old && cJSON_IsString(old)) {
                    double dv = strtod(old->valuestring, NULL);
                    cJSON_ReplaceItemInObject(params_obj, keys[i],
                        cJSON_CreateNumber(dv));
                }
                free(keys[i]);
            }
            free(keys);
        }
        char* normalized = cJSON_PrintUnformatted(params_obj);
        cJSON_AddStringToObject(payload, "paramsJSON",
            normalized ? normalized : params_str);
        free(normalized);
        cJSON_Delete(params_obj);
    } else {
        if (params_obj)
            cJSON_Delete(params_obj);
        cJSON_AddStringToObject(payload, "paramsJSON", params_str);
    }

    int send_ret = send_event(ws_fd, "node.invoke.request", payload);

    syslog(LOG_INFO, "[%s] Invoke %s on node %s (id=%.8s) fd=%d send=%d\n",
        TAG, command, node_id, invoke_id, ws_fd, send_ret);

    if (send_ret != 0) {
        syslog(LOG_ERR, "[%s] Failed to send invoke request to node %s\n",
            TAG, node_id);
        free_pending(p);
        snprintf(output, output_size, "Failed to send invoke to node '%s'", node_id);
        return ERROR;
    }

    /* Wait for result with timeout */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += INVOKE_TIMEOUT_S;

    pthread_mutex_lock(&p->mtx);
    while (!p->done) {
        int rc = pthread_cond_timedwait(&p->cond, &p->mtx, &ts);
        if (rc == ETIMEDOUT)
            break;
    }
    pthread_mutex_unlock(&p->mtx);

    int ret;
    if (!p->done) {
        syslog(LOG_WARNING, "[%s] Invoke %.8s TIMEOUT (fd=%d)\n", TAG, invoke_id,
            ws_fd);
        snprintf(output, output_size, "Node invoke timeout after %ds",
            INVOKE_TIMEOUT_S);
        ret = ERROR;
    } else if (p->ok) {
        syslog(LOG_INFO, "[%s] Invoke %.8s OK: %.200s\n", TAG, invoke_id,
            p->result_json ? p->result_json : "{}");
        strncpy(output, p->result_json ? p->result_json : "{}", output_size - 1);
        output[output_size - 1] = '\0';
        ret = OK;
    } else {
        syslog(LOG_WARNING, "[%s] Invoke %.8s FAILED: %s\n", TAG, invoke_id,
            p->result_json ? p->result_json : "unknown");
        snprintf(output, output_size, "Node error: %s",
            p->result_json ? p->result_json : "unknown");
        ret = ERROR;
    }

    free(p->result_json);
    free_pending(p);
    return ret;
}

char* node_manager_get_tools_json(void)
{
    cJSON* arr = cJSON_CreateArray();

    pthread_mutex_lock(&s_mtx);
    for (int i = 0; i < NODE_MAX_NODES; i++) {
        if (!s_nodes[i].active)
            continue;
        node_entry_t* n = &s_nodes[i];

        for (int j = 0; j < n->cmd_count; j++) {
            char full_name[128];
            snprintf(full_name, sizeof(full_name), "node.%s.%s", n->node_id,
                n->commands[j]);

            char desc[256];
            snprintf(desc, sizeof(desc), "[Remote %s/%s] %s on %s", n->platform,
                n->device_family, n->commands[j],
                n->display_name[0] ? n->display_name : n->node_id);

            cJSON* tool = cJSON_CreateObject();
            cJSON_AddStringToObject(tool, "name", full_name);
            cJSON_AddStringToObject(tool, "description", desc);
            cJSON* schema = cJSON_CreateObject();
            cJSON_AddStringToObject(schema, "type", "object");
            cJSON_AddItemToObject(schema, "properties", cJSON_CreateObject());
            cJSON_AddItemToObject(tool, "input_schema", schema);
            cJSON_AddItemToArray(arr, tool);
        }
    }
    pthread_mutex_unlock(&s_mtx);

    if (cJSON_GetArraySize(arr) == 0) {
        cJSON_Delete(arr);
        return NULL;
    }

    char* json = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return json;
}

int node_manager_list(char* buf, size_t size)
{
    int off = 0;
    int count = 0;

    pthread_mutex_lock(&s_mtx);
    for (int i = 0; i < NODE_MAX_NODES; i++) {
        if (!s_nodes[i].active)
            continue;
        node_entry_t* n = &s_nodes[i];
        count++;
        off += snprintf(buf + off, size - off, "  [%d] %s (%s/%s) fd=%d cmds=%d\n",
            i, n->node_id, n->platform, n->device_family, n->ws_fd,
            n->cmd_count);
        for (int j = 0; j < n->cmd_count && off < (int)size - 32; j++) {
            off += snprintf(buf + off, size - off, "      - %s\n", n->commands[j]);
        }
    }
    pthread_mutex_unlock(&s_mtx);

    if (count == 0) {
        snprintf(buf, size, "  (no nodes connected)\n");
    }
    return count;
}

/**
 * Send connect.challenge to a newly connected WebSocket client.
 * Called from ws_server when a new client connects, so Nodes
 * can identify themselves.
 */
void node_manager_send_challenge(int ws_fd)
{
    cJSON* payload = cJSON_CreateObject();
    char nonce[32];
    gen_id(nonce, sizeof(nonce));
    cJSON_AddStringToObject(payload, "nonce", nonce);
    send_event(ws_fd, "connect.challenge", payload);
}

int node_manager_active_count(void)
{
    int count = 0;
    pthread_mutex_lock(&s_mtx);
    for (int i = 0; i < NODE_MAX_NODES; i++) {
        if (s_nodes[i].active)
            count++;
    }
    pthread_mutex_unlock(&s_mtx);
    return count;
}

int node_manager_broadcast_chat(const char* channel, const char* chat_id,
    const char* content)
{
    if (!channel || !chat_id || !content)
        return 0;

    cJSON* payload = cJSON_CreateObject();
    cJSON_AddStringToObject(payload, "channel", channel);
    cJSON_AddStringToObject(payload, "chat_id", chat_id);
    cJSON_AddStringToObject(payload, "content", content);

    int sent = 0;
    pthread_mutex_lock(&s_mtx);
    for (int i = 0; i < NODE_MAX_NODES; i++) {
        if (s_nodes[i].active) {
            /* send_event takes ownership of payload, so duplicate for each node */
            cJSON* dup = cJSON_Duplicate(payload, true);
            if (dup && send_event(s_nodes[i].ws_fd, "chat.forward", dup) == 0) {
                syslog(LOG_INFO, "[%s] chat.forward → node %s\n", TAG,
                    s_nodes[i].node_id);
                sent++;
            } else {
                cJSON_Delete(dup);
            }
        }
    }
    pthread_mutex_unlock(&s_mtx);
    cJSON_Delete(payload);

    syslog(LOG_INFO, "[%s] Broadcast chat.forward to %d node(s)\n", TAG, sent);
    return sent;
}
