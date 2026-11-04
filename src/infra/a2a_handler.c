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
 * a2a_handler.c — A2A protocol handler (Server + Client).
 *
 * Server: handles /.well-known/agent.json, /a2a/invoke, /a2a/health
 * Client: discovers and invokes remote A2A agents via HTTPS
 */

#include "infra/a2a_handler.h"
#include "tools/tool_registry.h"
#include "core/message_bus.h"
#include "core/message_bus_tap.h"
#include "infra/vela_tls.h"
#include "agent_config.h"
#include "cJSON.h"

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <semaphore.h>
#include <sys/socket.h>

static const char *TAG = "a2a";

/* ══════════════════════════════════════════════════════════════
 *  Server Role
 * ══════════════════════════════════════════════════════════════ */

/** Send a complete HTTP response */
static void http_respond(int fd, int status, const char *status_text,
                         const char *content_type, const char *body)
{
    int body_len = body ? (int)strlen(body) : 0;
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n\r\n",
        status, status_text, content_type, body_len);
    if (hlen >= (int)sizeof(hdr))
        hlen = (int)sizeof(hdr) - 1;
    send(fd, hdr, hlen, 0);
    if (body && body_len > 0)
        send(fd, body, body_len, 0);
}

/** GET /.well-known/agent.json */
static void handle_agent_card(int fd)
{
    char *tools_json = tool_registry_get_tools_json();

    cJSON *card = cJSON_CreateObject();
    if (!card) {
        http_respond(fd, 500, "Internal Server Error",
                     "application/json", "{\"error\":\"OOM\"}");
        free(tools_json);
        return;
    }

    cJSON_AddStringToObject(card, "name",
        CONFIG_EXAMPLES_AI_AGENT_VELA_PROGNAME);
    cJSON_AddStringToObject(card, "description",
        "AI Agent on Vela/NuttX");
    cJSON_AddStringToObject(card, "protocolVersion", "2025-03-26");

    cJSON *caps = cJSON_AddObjectToObject(card, "capabilities");
    if (caps) {
        cJSON_AddBoolToObject(caps, "streaming", false);
        cJSON_AddBoolToObject(caps, "pushNotifications", false);
    }

    /* Build skills from tool_registry */
    cJSON *skills = cJSON_AddArrayToObject(card, "skills");
    if (skills && tools_json) {
        cJSON *tools = cJSON_Parse(tools_json);
        if (tools) {
            cJSON *t;
            cJSON_ArrayForEach(t, tools) {
                cJSON *name = cJSON_GetObjectItem(t, "name");
                cJSON *desc = cJSON_GetObjectItem(t, "description");
                if (cJSON_IsString(name)) {
                    cJSON *skill = cJSON_CreateObject();
                    if (skill) {
                        cJSON_AddStringToObject(skill, "id",
                            name->valuestring);
                        cJSON_AddStringToObject(skill, "name",
                            name->valuestring);
                        if (cJSON_IsString(desc))
                            cJSON_AddStringToObject(skill, "description",
                                desc->valuestring);
                        cJSON_AddItemToArray(skills, skill);
                    }
                }
            }
            cJSON_Delete(tools);
        }
    }
    free(tools_json);

    char *json = cJSON_PrintUnformatted(card);
    cJSON_Delete(card);

    if (json) {
        syslog(LOG_INFO, "[%s] AgentCard served (%d bytes)\n",
               TAG, (int)strlen(json));
        http_respond(fd, 200, "OK", "application/json", json);
        free(json);
    } else {
        http_respond(fd, 500, "Internal Server Error",
                     "application/json", "{\"error\":\"OOM\"}");
    }
}

/* ── Synchronous agent invoke via message_bus + tap ────────── */

/** Context for waiting on agent response (heap-allocated for safety) */
typedef struct {
    sem_t sem;
    char *response;  /* heap-allocated, caller frees */
    bool done;       /* set by caller after sem_timedwait returns */
} a2a_invoke_ctx_t;

/** Tap callback — copies response and signals semaphore */
static void a2a_tap_cb(const agent_msg_t *msg, void *cookie)
{
    a2a_invoke_ctx_t *ctx = (a2a_invoke_ctx_t *)cookie;
    if (ctx->done) return; /* caller already timed out and moved on */
    if (msg->content)
        ctx->response = strdup(msg->content);
    sem_post(&ctx->sem);
}

/** POST /a2a/invoke — Send message to agent_loop, wait for response */
static void handle_invoke(int fd, const char *body)
{
    cJSON *req = cJSON_Parse(body);
    if (!req) {
        http_respond(fd, 400, "Bad Request",
                     "application/json", "{\"error\":\"invalid JSON\"}");
        return;
    }

    /* Accept both formats:
     * A2A style: {"message":"text"} or {"message":{"parts":[{"text":"..."}]}}
     * Simple:    {"skill_id":"...", "args":{...}} (direct tool call) */
    const char *text = NULL;
    char *text_buf = NULL;

    cJSON *msg_field = cJSON_GetObjectItem(req, "message");
    if (cJSON_IsString(msg_field)) {
        text = msg_field->valuestring;
    } else if (cJSON_IsObject(msg_field)) {
        cJSON *parts = cJSON_GetObjectItem(msg_field, "parts");
        if (cJSON_IsArray(parts)) {
            cJSON *part;
            cJSON_ArrayForEach(part, parts) {
                cJSON *t = cJSON_GetObjectItem(part, "text");
                if (cJSON_IsString(t)) { text = t->valuestring; break; }
            }
        }
    }

    /* Fallback: direct tool call format */
    if (!text) {
        cJSON *skill = cJSON_GetObjectItem(req, "skill_id");
        cJSON *args = cJSON_GetObjectItem(req, "args");
        if (cJSON_IsString(skill)) {
            char *args_str = args ? cJSON_PrintUnformatted(args) : NULL;
            size_t len = strlen(skill->valuestring)
                         + (args_str ? strlen(args_str) : 0) + 32;
            text_buf = malloc(len);
            if (text_buf) {
                if (args_str && strcmp(args_str, "{}") != 0)
                    snprintf(text_buf, len, "Call tool %s with %s",
                             skill->valuestring, args_str);
                else
                    snprintf(text_buf, len, "Call tool %s",
                             skill->valuestring);
                text = text_buf;
            }
            free(args_str);
        }
    }

    if (!text) {
        cJSON_Delete(req);
        http_respond(fd, 400, "Bad Request", "application/json",
                     "{\"error\":\"missing message or skill_id\"}");
        return;
    }

    syslog(LOG_INFO, "[%s] invoke via agent: %.64s\n", TAG, text);

    /* Heap-allocate ctx so tap callback is safe even after timeout */
    a2a_invoke_ctx_t *ctx = calloc(1, sizeof(a2a_invoke_ctx_t));
    if (!ctx) {
        cJSON_Delete(req);
        free(text_buf);
        http_respond(fd, 500, "Internal Server Error", "application/json",
                     "{\"error\":\"OOM\"}");
        return;
    }
    sem_init(&ctx->sem, 0, 0);

    /* Use per-request tap key to support concurrent requests */
    char tap_key[16];
    snprintf(tap_key, sizeof(tap_key), "a2a_%d", fd);

    if (mbus_tap_register(tap_key, a2a_tap_cb, ctx) != OK) {
        cJSON_Delete(req);
        free(text_buf);
        sem_destroy(&ctx->sem);
        free(ctx);
        http_respond(fd, 503, "Service Unavailable", "application/json",
                     "{\"error\":\"agent busy\"}");
        return;
    }

    /* Push message to agent_loop */
    agent_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, tap_key, sizeof(msg.channel) - 1);
    snprintf(msg.chat_id, sizeof(msg.chat_id), "a2a_%d", fd);
    msg.content = strdup(text);

    cJSON_Delete(req);
    free(text_buf);

    if (!msg.content || message_bus_push_inbound(&msg) != OK) {
        free(msg.content);
        mbus_tap_unregister(tap_key);
        sem_destroy(&ctx->sem);
        free(ctx);
        http_respond(fd, 500, "Internal Server Error", "application/json",
                     "{\"error\":\"queue full\"}");
        return;
    }

    /* Wait for agent response (timeout 30s) */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += 30;

    int wait_rc = sem_timedwait(&ctx->sem, &ts);

    /* Mark done so late tap callbacks are no-ops */
    ctx->done = true;
    mbus_tap_unregister(tap_key);

    if (wait_rc != 0 || !ctx->response) {
        free(ctx->response);
        sem_destroy(&ctx->sem);
        free(ctx);
        http_respond(fd, 504, "Gateway Timeout", "application/json",
                     "{\"error\":\"agent timeout\"}");
        return;
    }

    /* Build A2A response with Message format */
    cJSON *resp = cJSON_CreateObject();
    if (!resp) {
        free(ctx->response);
        sem_destroy(&ctx->sem);
        free(ctx);
        http_respond(fd, 500, "Internal Server Error", "application/json",
                     "{\"error\":\"OOM\"}");
        return;
    }
    cJSON_AddStringToObject(resp, "status", "completed");
    cJSON *out_msg = cJSON_AddObjectToObject(resp, "message");
    if (out_msg) {
        cJSON_AddStringToObject(out_msg, "role", "agent");
        cJSON *parts = cJSON_AddArrayToObject(out_msg, "parts");
        if (parts) {
            cJSON *part = cJSON_CreateObject();
            if (part) {
                cJSON_AddStringToObject(part, "type", "text");
                cJSON_AddStringToObject(part, "text", ctx->response);
                cJSON_AddItemToArray(parts, part);
            }
        }
    }

    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    free(ctx->response);
    sem_destroy(&ctx->sem);
    free(ctx);

    if (json) {
        http_respond(fd, 200, "OK", "application/json", json);
        free(json);
    } else {
        http_respond(fd, 500, "Internal Server Error", "application/json",
                     "{\"error\":\"OOM\"}");
    }
}

/** GET /a2a/health */
static void handle_health(int fd)
{
    http_respond(fd, 200, "OK", "application/json",
                 "{\"status\":\"ok\"}");
}

/* ── Server entry point ───────────────────────────────────── */

bool a2a_try_handle(int fd, const char *buf, int buf_len)
{
    (void)buf_len;

    /* WebSocket upgrade — not ours */
    if (strcasestr(buf, "Upgrade: websocket"))
        return false;

    /* Parse method and path from "GET /path HTTP/1.1" */
    const char *method_end = strchr(buf, ' ');
    if (!method_end) return false;
    const char *path_start = method_end + 1;
    const char *path_end = strchr(path_start, ' ');
    if (!path_end) return false;

    size_t method_len = (size_t)(method_end - buf);
    char path[128];
    size_t path_len = (size_t)(path_end - path_start);
    if (path_len >= sizeof(path)) path_len = sizeof(path) - 1;
    memcpy(path, path_start, path_len);
    path[path_len] = '\0';

    bool is_get = (method_len == 3 && strncmp(buf, "GET", 3) == 0);
    bool is_post = (method_len == 4 && strncmp(buf, "POST", 4) == 0);

    if (is_get && strcmp(path, "/.well-known/agent.json") == 0) {
        handle_agent_card(fd);
        return true;
    }
    if (is_get && strcmp(path, "/a2a/health") == 0) {
        handle_health(fd);
        return true;
    }
    if (is_post && strcmp(path, "/a2a/invoke") == 0) {
        const char *body_start = strstr(buf, "\r\n\r\n");
        if (!body_start) {
            http_respond(fd, 400, "Bad Request",
                         "application/json", "{\"error\":\"no body\"}");
            return true;
        }
        body_start += 4;

        /* Read remaining body from fd if not fully in peek buffer */
        int body_in_buf = buf_len - (int)(body_start - buf);
        char body[4096];
        if (body_in_buf > 0 && body_in_buf < (int)sizeof(body)) {
            memcpy(body, body_start, body_in_buf);
        } else {
            body_in_buf = 0;
        }

        /* Check Content-Length and read more if needed */
        const char *cl = strcasestr(buf, "\r\nContent-Length: ");
        int content_len = 0;
        if (cl) content_len = atoi(cl + 18);
        if (content_len > (int)sizeof(body) - 1)
            content_len = (int)sizeof(body) - 1;

        while (body_in_buf < content_len) {
            int n = recv(fd, body + body_in_buf,
                         content_len - body_in_buf, 0);
            if (n <= 0) break;
            body_in_buf += n;
        }
        body[body_in_buf] = '\0';

        handle_invoke(fd, body);
        return true;
    }

    return false;
}

/* ══════════════════════════════════════════════════════════════
 *  Client Role
 * ══════════════════════════════════════════════════════════════ */

char *a2a_client_discover(const char *host, const char *port)
{
    if (!host || !port) return NULL;

    char *resp = malloc(4096);
    if (!resp) return NULL;

    int status = vela_https_get(host, port,
        "/.well-known/agent.json", resp, 4096);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] discover %s:%s failed: HTTP %d\n",
               TAG, host, port, status);
        free(resp);
        return NULL;
    }

    syslog(LOG_INFO, "[%s] discovered agent at %s:%s\n", TAG, host, port);
    return resp;
}

char *a2a_client_invoke(const char *host, const char *port,
                        const char *skill_id, const char *args_json)
{
    if (!host || !port || !skill_id) return NULL;

    /* Build request body */
    cJSON *req = cJSON_CreateObject();
    if (!req) return NULL;
    cJSON_AddStringToObject(req, "skill_id", skill_id);

    if (args_json) {
        cJSON *args = cJSON_Parse(args_json);
        if (args)
            cJSON_AddItemToObject(req, "args", args);
        else
            cJSON_AddStringToObject(req, "args", args_json);
    }

    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return NULL;

    vela_header_t hdrs[] = {
        { "Content-Type", "application/json" },
        { NULL, NULL },
    };

    char *resp = malloc(8192);
    if (!resp) { free(body); return NULL; }

    int status = vela_https_request(host, port, "POST", "/a2a/invoke",
        hdrs, body, strlen(body), resp, 8192, NULL);
    free(body);

    if (status < 200 || status >= 300) {
        syslog(LOG_ERR, "[%s] invoke %s:%s/%s failed: HTTP %d\n",
               TAG, host, port, skill_id, status);
        free(resp);
        return NULL;
    }

    syslog(LOG_INFO, "[%s] invoked %s on %s:%s\n",
           TAG, skill_id, host, port);
    return resp;
}

int a2a_client_health(const char *host, const char *port)
{
    if (!host || !port) return -EINVAL;

    char resp[256];
    int status = vela_https_get(host, port, "/a2a/health", resp, sizeof(resp));

    if (status != 200)
        return -EIO;

    cJSON *root = cJSON_Parse(resp);
    if (!root) return -EINVAL;

    cJSON *st = cJSON_GetObjectItem(root, "status");
    int ok = (cJSON_IsString(st) && strcmp(st->valuestring, "ok") == 0)
             ? 0 : -EIO;
    cJSON_Delete(root);
    return ok;
}
