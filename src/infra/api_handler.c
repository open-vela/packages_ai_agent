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

#include "infra/api_handler.h"
#include "agent_config.h"
#include "infra/config_store.h"
#include "llm/llm_router.h"
#include "tools/skill_loader.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cJSON.h"

static const char* TAG = "api";

/* Config keys exposed via REST API */

static const char* s_config_keys[] = {
    AGENT_CFG_KEY_API_KEY,
    AGENT_CFG_KEY_MODEL,
    AGENT_CFG_KEY_LLM_HOST,
    AGENT_CFG_KEY_LLM_PATH,
    AGENT_CFG_KEY_TAVILY_KEY,
    AGENT_CFG_KEY_VOLC_APPKEY,
    AGENT_CFG_KEY_VOLC_TOKEN,
    AGENT_CFG_KEY_VOLC_API_KEY,
    AGENT_CFG_KEY_VOLC_CLUSTER,
    AGENT_CFG_KEY_VOLC_ASR_CLUSTER,
    AGENT_CFG_KEY_PROXY_HOST,
    AGENT_CFG_KEY_PROXY_PORT,
};

#define NUM_CONFIG_KEYS (sizeof(s_config_keys) / sizeof(s_config_keys[0]))

/* ── HTTP helpers ─────────────────────────────────────────────── */

static void send_response(int fd, int code, const char* body)
{
    const char* status = (code == 200) ? "OK" : "Bad Request";
    char hdr[256];
    int hlen = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 %d %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n\r\n",
        code, status, (int)strlen(body));
    send(fd, hdr, hlen, 0);
    send(fd, body, strlen(body), 0);
}

static const char* find_body(const char* buf, int buf_len)
{
    const char* p = strstr(buf, "\r\n\r\n");
    return p ? p + 4 : NULL;
}

/**
 * Read full HTTP body. peek_buf may contain partial body after headers.
 * Reads remaining bytes from fd based on Content-Length.
 * Returns malloc'd buffer (caller must free), or NULL on failure.
 */
static char* read_full_body(int fd, const char* buf, int buf_len)
{
    const char* hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end)
        return NULL;
    hdr_end += 4;

    /* Find Content-Length */
    const char* cl = strcasestr(buf, "\r\nContent-Length: ");
    int content_len = cl ? atoi(cl + 18) : 0;
    if (content_len <= 0 || content_len > 32 * 1024) {
        /* No Content-Length or too large — use what's already in buffer */
        int already = buf_len - (int)(hdr_end - buf);
        if (already <= 0)
            return NULL;
        char* body = malloc(already + 1);
        if (!body)
            return NULL;
        memcpy(body, hdr_end, already);
        body[already] = '\0';
        return body;
    }

    char* body = malloc(content_len + 1);
    if (!body)
        return NULL;

    /* Copy what's already peeked */
    int already = buf_len - (int)(hdr_end - buf);
    if (already > content_len)
        already = content_len;
    if (already > 0)
        memcpy(body, hdr_end, already);

    /* Read remaining from socket */
    int remaining = content_len - already;
    int offset = already;
    while (remaining > 0) {
        int n = recv(fd, body + offset, remaining, 0);
        if (n <= 0) {
            free(body);
            return NULL;
        }
        offset += n;
        remaining -= n;
    }

    body[content_len] = '\0';
    return body;
}

/* ── Config handlers ──────────────────────────────────────────── */

/* Keys that live inside llm_backend_0 JSON (not flat config) */

static const char* s_backend_key_map[][2] = {
    { "api_key", "api_key" },
    { "model", "model" },
    { "llm_host", "host" },
    { "llm_path", "path" },
};

#define NUM_BACKEND_KEYS (sizeof(s_backend_key_map) / sizeof(s_backend_key_map[0]))

static bool is_backend_key(const char* key, const char** backend_field)
{
    for (int i = 0; i < (int)NUM_BACKEND_KEYS; i++) {
        if (strcmp(key, s_backend_key_map[i][0]) == 0) {
            *backend_field = s_backend_key_map[i][1];
            return true;
        }
    }
    return false;
}

/* Keys whose values are secrets and must be masked in GET responses */

static const char* s_secret_keys[] = {
    AGENT_CFG_KEY_API_KEY,
    AGENT_CFG_KEY_TAVILY_KEY,
    AGENT_CFG_KEY_VOLC_APPKEY,
    AGENT_CFG_KEY_VOLC_TOKEN,
    AGENT_CFG_KEY_VOLC_API_KEY,
};

#define NUM_SECRET_KEYS (sizeof(s_secret_keys) / sizeof(s_secret_keys[0]))

static bool is_secret_key(const char* key)
{
    for (int i = 0; i < (int)NUM_SECRET_KEYS; i++) {
        if (strcmp(key, s_secret_keys[i]) == 0)
            return true;
    }
    return false;
}

/**
 * Mask a secret value for display. Keeps the first and last 2 characters
 * and replaces the middle with "****" so the companion App can tell
 * whether a key is configured without exposing the actual secret over
 * the wire. Values of 4 characters or fewer are fully masked. An empty
 * value stays empty.
 */
static void mask_value(const char* in, char* out, size_t out_size)
{
    size_t len = in ? strlen(in) : 0;
    if (len == 0) {
        if (out_size > 0)
            out[0] = '\0';
        return;
    }
    if (len <= 4) {
        snprintf(out, out_size, "****");
        return;
    }
    snprintf(out, out_size, "%c%c****%c%c",
        in[0], in[1], in[len - 2], in[len - 1]);
}

static bool handle_config_get(int fd)
{
    cJSON* obj = cJSON_CreateObject();
    char val[512];
    char masked[32];

    /* Read llm_backend_0 and extract LLM fields */
    char backend_json[1024] = { 0 };
    cJSON* backend_obj = NULL;
    if (claw_config_get("llm_backend_0", backend_json, sizeof(backend_json)) == OK) {
        backend_obj = cJSON_Parse(backend_json);
    }

    for (int i = 0; i < (int)NUM_CONFIG_KEYS; i++) {
        const char* backend_field = NULL;
        const char* raw = "";
        if (is_backend_key(s_config_keys[i], &backend_field) && backend_obj) {
            cJSON* item = cJSON_GetObjectItem(backend_obj, backend_field);
            raw = (item && cJSON_IsString(item)) ? item->valuestring : "";
        } else {
            val[0] = '\0';
            if (claw_config_get(s_config_keys[i], val, sizeof(val)) == OK)
                raw = val;
            else
                raw = "";
        }

        if (is_secret_key(s_config_keys[i])) {
            mask_value(raw, masked, sizeof(masked));
            cJSON_AddStringToObject(obj, s_config_keys[i], masked);
        } else {
            cJSON_AddStringToObject(obj, s_config_keys[i], raw);
        }
    }

    if (backend_obj)
        cJSON_Delete(backend_obj);

    char* json = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (!json) {
        send_response(fd, 500, "{\"error\":\"out of memory\"}");
        return true;
    }

    send_response(fd, 200, json);
    free(json);
    return true;
}

static bool handle_config_put(int fd, const char* body)
{
    syslog(LOG_INFO, "[%s] PUT /api/config body=%s\n", TAG,
        body ? body : "(null)");

    if (!body || *body == '\0') {
        syslog(LOG_WARNING, "[%s] PUT config: empty body\n", TAG);
        send_response(fd, 400, "{\"error\":\"empty body\"}");
        return true;
    }

    cJSON* req = cJSON_Parse(body);
    if (!req) {
        send_response(fd, 400, "{\"error\":\"invalid json\"}");
        return true;
    }

    /* Collect backend field updates */
    bool backend_dirty = false;
    char backend_json[1024] = { 0 };
    claw_config_get("llm_backend_0", backend_json, sizeof(backend_json));
    cJSON* backend_obj = cJSON_Parse(backend_json);
    if (!backend_obj) {
        backend_obj = cJSON_CreateObject();
    }

    cJSON* item = NULL;
    cJSON_ArrayForEach(item, req)
    {
        if (!cJSON_IsString(item) || !item->string) {
            continue;
        }
        /* Reject path traversal */
        if (strchr(item->string, '/') || strstr(item->string, "..")) {
            continue;
        }

        /* Skip secret fields whose value is still the GET mask, so a
         * client that read masked values and PUTs the whole object back
         * does not overwrite the stored secret with "****".
         */
        if (is_secret_key(item->string) && item->valuestring
            && strstr(item->valuestring, "****")) {
            continue;
        }

        const char* backend_field = NULL;
        if (is_backend_key(item->string, &backend_field)) {
            /* Update inside llm_backend_0 */
            cJSON_DeleteItemFromObject(backend_obj, backend_field);
            cJSON_AddStringToObject(backend_obj, backend_field, item->valuestring);
            backend_dirty = true;
            /* Also write flat key so config_show can display it */
            claw_config_set(item->string, item->valuestring);
        } else {
            /* Flat config key */
            claw_config_set(item->string, item->valuestring);
        }
    }

    /* Persist backend changes */
    if (backend_dirty) {
        char* bstr = cJSON_PrintUnformatted(backend_obj);
        if (bstr) {
            claw_config_set("llm_backend_0", bstr);
            free(bstr);
        }
        /* Reload LLM router so changes take effect immediately */
        llm_router_init();
    }

    cJSON_Delete(backend_obj);
    cJSON_Delete(req);
    send_response(fd, 200, "{\"ok\":true}");
    return true;
}

/* ── Skills handlers ──────────────────────────────────────────── */

static void url_decode(const char* src, char* dst, size_t dst_size)
{
    size_t di = 0;
    for (size_t si = 0; src[si] && di < dst_size - 1; si++) {
        if (src[si] == '%' && src[si + 1] && src[si + 2]) {
            char hex[3] = { src[si + 1], src[si + 2], 0 };
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
        } else if (src[si] == '+') {
            dst[di++] = ' ';
        } else {
            dst[di++] = src[si];
        }
    }
    dst[di] = '\0';
}

static bool handle_skills_get(int fd)
{
    cJSON* arr = cJSON_CreateArray();
    DIR* dir = opendir(AGENT_SKILLS_DIR);

    if (dir) {
        struct dirent* ent;
        while ((ent = readdir(dir)) != NULL) {
            size_t nlen = strlen(ent->d_name);
            if (nlen < 4 || strcmp(ent->d_name + nlen - 3, ".md") != 0) {
                continue;
            }

            /* Build full path */
            char path[256];
            snprintf(path, sizeof(path), "%s%s", AGENT_SKILLS_DIR,
                ent->d_name);

            struct stat st;
            if (stat(path, &st) != 0) {
                continue;
            }

            /* Extract name (without .md) */
            char name[128];
            snprintf(name, sizeof(name), "%.*s", (int)(nlen - 3),
                ent->d_name);

            /* Try to read description and content */
            char desc[256] = "";
            char content[2048] = "";
            FILE* f = fopen(path, "r");
            if (f) {
                char line[256];
                bool in_front = false;
                int content_len = 0;
                while (fgets(line, sizeof(line), f)) {
                    if (strncmp(line, "---", 3) == 0) {
                        in_front = !in_front;
                        continue;
                    }
                    if (in_front && strncmp(line, "description:", 12) == 0) {
                        char* v = line + 12;
                        while (*v == ' ')
                            v++;
                        size_t vlen = strlen(v);
                        if (vlen > 0 && v[vlen - 1] == '\n')
                            v[vlen - 1] = '\0';
                        snprintf(desc, sizeof(desc), "%s", v);
                    }
                    /* Accumulate content */
                    size_t ll = strlen(line);
                    if (content_len + (int)ll < (int)sizeof(content) - 1) {
                        memcpy(content + content_len, line, ll);
                        content_len += (int)ll;
                    }
                }
                content[content_len] = '\0';
                fclose(f);
            }

            cJSON* skill = cJSON_CreateObject();
            cJSON_AddStringToObject(skill, "name", name);
            cJSON_AddStringToObject(skill, "description", desc);
            cJSON_AddStringToObject(skill, "content", content);
            cJSON_AddStringToObject(skill, "file", ent->d_name);
            cJSON_AddNumberToObject(skill, "size", (double)st.st_size);

            /* mtime as ISO string */
            struct tm tm;
            localtime_r(&st.st_mtime, &tm);
            char mtime[32];
            snprintf(mtime, sizeof(mtime), "%04d-%02d-%02dT%02d:%02d:%02d",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec);
            cJSON_AddStringToObject(skill, "mtime", mtime);

            cJSON_AddItemToArray(arr, skill);
        }
        closedir(dir);
    }

    cJSON* resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "skills", arr);
    char* json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (!json) {
        send_response(fd, 500, "{\"error\":\"out of memory\"}");
        return true;
    }

    send_response(fd, 200, json);
    free(json);
    return true;
}

static bool handle_skills_post(int fd, const char* body)
{
    if (!body || *body == '\0') {
        send_response(fd, 400, "{\"error\":\"empty body\"}");
        return true;
    }

    cJSON* req = cJSON_Parse(body);
    if (!req) {
        send_response(fd, 400, "{\"error\":\"invalid json\"}");
        return true;
    }

    cJSON* name_item = cJSON_GetObjectItem(req, "name");
    cJSON* content_item = cJSON_GetObjectItem(req, "content");

    if (!cJSON_IsString(name_item) || !cJSON_IsString(content_item)) {
        cJSON_Delete(req);
        send_response(fd, 400, "{\"error\":\"name and content required\"}");
        return true;
    }

    const char* name = name_item->valuestring;
    const char* content = content_item->valuestring;

    /* Validate name: no path traversal */
    if (strchr(name, '/') || strchr(name, '\\') || strstr(name, "..")) {
        cJSON_Delete(req);
        send_response(fd, 400, "{\"error\":\"invalid skill name\"}");
        return true;
    }

    /* Write file */
    char path[256];
    snprintf(path, sizeof(path), "%s%s.md", AGENT_SKILLS_DIR, name);

    FILE* f = fopen(path, "w");
    if (!f) {
        cJSON_Delete(req);
        send_response(fd, 500, "{\"error\":\"cannot write file\"}");
        return true;
    }
    fputs(content, f);
    fclose(f);
    cJSON_Delete(req);

    /* Hot-reload skills */
    skill_loader_refresh();

    syslog(LOG_INFO, "[%s] Skill pushed: %s\n", TAG, name);
    send_response(fd, 200, "{\"ok\":true}");
    return true;
}

static bool handle_skills_delete(int fd, const char* name)
{
    if (!name || *name == '\0') {
        send_response(fd, 400, "{\"error\":\"skill name required\"}");
        return true;
    }

    /* URL decode the name (browser/OkHttp encodes Chinese chars) */
    char decoded[128];
    url_decode(name, decoded, sizeof(decoded));

    /* Validate name */
    if (strchr(decoded, '/') || strchr(decoded, '\\') || strstr(decoded, "..")) {
        send_response(fd, 400, "{\"error\":\"invalid skill name\"}");
        return true;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s%s.md", AGENT_SKILLS_DIR, decoded);

    if (unlink(path) != 0 && errno != ENOENT) {
        send_response(fd, 500, "{\"error\":\"cannot delete file\"}");
        return true;
    }

    skill_loader_refresh();

    syslog(LOG_INFO, "[%s] Skill deleted: %s\n", TAG, decoded);
    send_response(fd, 200, "{\"ok\":true}");
    return true;
}

/* ── Main dispatch ────────────────────────────────────────────── */

/* ── Logs handler ─────────────────────────────────────────────── */

static bool handle_logs_get(int fd)
{
    cJSON* arr = agent_logbuf_dump();
    cJSON* resp = cJSON_CreateObject();
    cJSON_AddItemToObject(resp, "logs", arr);
    char* json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);

    if (!json) {
        send_response(fd, 500, "{\"error\":\"out of memory\"}");
        return true;
    }

    send_response(fd, 200, json);
    free(json);
    return true;
}

/* ── Main dispatch ────────────────────────────────────────────── */

bool api_try_handle(int fd, const char* buf, int buf_len)
{
    /* Quick check: must start with a valid HTTP method targeting /api/ */
    if (!buf || buf_len < 10) {
        return false;
    }

    if (strncmp(buf, "GET /api/", 9) != 0 && strncmp(buf, "PUT /api/", 9) != 0 && strncmp(buf, "POST /api/", 10) != 0 && strncmp(buf, "DELETE /api/", 12) != 0) {
        return false;
    }

    /* Parse method and path */
    char method[8] = "";
    char path[128] = "";
    sscanf(buf, "%7s %127s", method, path);

    syslog(LOG_INFO, "[%s] api_try_handle: %s %s\n", TAG, method, path);

    /* Route: /api/config */
    if (strcmp(path, "/api/config") == 0) {
        if (strcmp(method, "GET") == 0) {
            return handle_config_get(fd);
        }
        if (strcmp(method, "PUT") == 0) {
            char* body = read_full_body(fd, buf, buf_len);
            bool ret = handle_config_put(fd, body);
            free(body);
            return ret;
        }
    }

    /* Route: /api/skills */
    if (strcmp(path, "/api/skills") == 0) {
        if (strcmp(method, "GET") == 0) {
            return handle_skills_get(fd);
        }
        if (strcmp(method, "POST") == 0) {
            char* body = read_full_body(fd, buf, buf_len);
            bool ret = handle_skills_post(fd, body);
            free(body);
            return ret;
        }
    }

    /* Route: /api/skills/{name} */
    if (strncmp(path, "/api/skills/", 12) == 0 && strcmp(method, "DELETE") == 0) {
        return handle_skills_delete(fd, path + 12);
    }

    /* Route: /api/logs */
    if (strncmp(path, "/api/logs", 9) == 0 && strcmp(method, "GET") == 0) {
        return handle_logs_get(fd);
    }

    return false;
}
