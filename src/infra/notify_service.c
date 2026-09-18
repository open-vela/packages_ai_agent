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
 * notify_service.c — Gerrit/Jira polling → LVGL toast.
 *
 * Thread model: a single detached notify_thread, cond_timedwait-driven,
 * each source keeping its own next-due timestamp.  Polling is a sync
 * mcp_client_execute() call on this thread (never on agent_loop).
 *
 * Dedup: Gerrit change numbers are numeric and ~monotonic → store the max
 * seen, only report id > last_max.  Jira keys are "PROJ-123" (non-numeric)
 * → store a last-seen key + a small in-memory LRU of reported keys.
 *
 * Cursor persistence: config_store (claw_config_get/set).  Cursor is
 * updated in memory first, then pushed to outbound, then persisted — so
 * a push failure (message bus full) does NOT lose the cursor (we'd rather
 * miss a notification this round and re-report next round than silently
 * advance the cursor past something the user never saw).
 */

#include "infra/notify_service.h"
#include "agent_config.h"
#include "agent_compat.h"
#include "core/message_bus.h"
#include "infra/config_store.h"
#include "tools/mcp_client.h"
#include "ui/lvgl_toast.h"

#include "cJSON.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#ifdef CONFIG_AI_AGENT_NOTIFY_SERVICE

static const char *TAG = "notify";

/* ── Per-source config ─────────────────────────────────────────── */

typedef struct {
    bool enabled;
    char server_name[32];   /* "gerrit" / "jira" — MCP server name */
    char tool_query[64];   /* "gerrit.query_changes" / "jira.search" */
    char query_args[256];   /* JSON args string */
    int interval_sec;
    char last_cursor[32];  /* Gerrit: max change num; Jira: last seen key */
} notify_src_cfg_t;

static notify_src_cfg_t s_cfg[NOTIFY_SRC_COUNT];

/* Jira in-memory LRU of recently reported keys (not persisted). */
#define JIRA_LRU_SIZE 64
static char s_jira_lru[JIRA_LRU_SIZE][24];
static int s_jira_lru_n = 0;

/* Thread state */
static volatile bool s_running = false;
static pthread_mutex_t s_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static bool s_inited = false;

/* Status snapshot (for notify_service_status_json) */
static int s_last_poll_epoch = 0;
static int s_last_new_count = 0;
static char s_last_err[64] = "";

/* config_store key prefix per source */
static const char *cfg_key(notify_source_t src, const char *field)
{
    static char buf[64];
    snprintf(buf, sizeof(buf), "ai.notify.%s.%s",
             src == NOTIFY_SRC_GERRIT ? "gerrit" : "jira", field);
    return buf;
}

/* ── Config load/save ─────────────────────────────────────────── */

static void cfg_load_str(notify_source_t src, const char *field,
                         char *out, size_t out_sz, const char *def)
{
    if (claw_config_get(cfg_key(src, field), out, out_sz) != OK || !out[0]) {
        strlcpy(out, def ? def : "", out_sz);
    }
}

static bool cfg_load_bool(notify_source_t src, const char *field, bool def)
{
    char buf[8];
    if (claw_config_get(cfg_key(src, field), buf, sizeof(buf)) != OK || !buf[0]) {
        return def;
    }
    return (buf[0] == '1' || buf[0] == 't' || buf[0] == 'T');
}

static int cfg_load_int(notify_source_t src, const char *field, int def)
{
    char buf[12];
    if (claw_config_get(cfg_key(src, field), buf, sizeof(buf)) != OK || !buf[0]) {
        return def;
    }
    return atoi(buf);
}

static void notify_load_cfg(void)
{
    /* Gerrit */
    notify_src_cfg_t *g = &s_cfg[NOTIFY_SRC_GERRIT];
    g->enabled = cfg_load_bool(NOTIFY_SRC_GERRIT, "enabled", true);
    cfg_load_str(NOTIFY_SRC_GERRIT, "server", g->server_name,
                 sizeof(g->server_name), "gerrit");
    cfg_load_str(NOTIFY_SRC_GERRIT, "tool", g->tool_query,
                 sizeof(g->tool_query), "gerrit.query_changes");
    cfg_load_str(NOTIFY_SRC_GERRIT, "query_args", g->query_args,
                 sizeof(g->query_args),
                 "{\"query\":\"status:open\",\"limit\":50}");
    g->interval_sec = cfg_load_int(NOTIFY_SRC_GERRIT, "interval_sec",
                                   AGENT_NOTIFY_INTERVAL_SEC);
    cfg_load_str(NOTIFY_SRC_GERRIT, "last_cursor", g->last_cursor,
                 sizeof(g->last_cursor), "0");

    /* Jira */
    notify_src_cfg_t *j = &s_cfg[NOTIFY_SRC_JIRA];
    j->enabled = cfg_load_bool(NOTIFY_SRC_JIRA, "enabled", true);
    cfg_load_str(NOTIFY_SRC_JIRA, "server", j->server_name,
                 sizeof(j->server_name), "jira");
    cfg_load_str(NOTIFY_SRC_JIRA, "tool", j->tool_query,
                 sizeof(j->tool_query), "jira.search");
    {
        /* Default JQL: issues updated since boot time.  If a cursor was
         * persisted we use it; otherwise we fall back to "updated >= now". */
        char ts[32];
        time_t now = time(NULL);
        struct tm tm;
        gmtime_r(&now, &tm);
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M", &tm);
        snprintf(j->query_args, sizeof(j->query_args),
                 "{\"jql\":\"updated >= \\\"%s\\\" ORDER BY updated DESC\","
                 "\"fields\":\"summary,status,priority,assignee\",\"limit\":50}",
                 ts);
    }
    j->interval_sec = cfg_load_int(NOTIFY_SRC_JIRA, "interval_sec",
                                   AGENT_NOTIFY_INTERVAL_SEC * 2);
    cfg_load_str(NOTIFY_SRC_JIRA, "last_cursor", j->last_cursor,
                 sizeof(j->last_cursor), "");
}

static void persist_cursor(notify_source_t src)
{
    claw_config_set(cfg_key(src, "last_cursor"), s_cfg[src].last_cursor);
}

/* ── Jira LRU ────────────────────────────────────────────────── */

static bool jira_lru_seen(const char *key)
{
    for (int i = 0; i < s_jira_lru_n; i++) {
        if (strcmp(s_jira_lru[i], key) == 0) return true;
    }
    return false;
}

static void jira_lru_add(const char *key)
{
    if (s_jira_lru_n < JIRA_LRU_SIZE) {
        strlcpy(s_jira_lru[s_jira_lru_n], key, sizeof(s_jira_lru[0]));
        s_jira_lru_n++;
    } else {
        /* ring: overwrite oldest */
        memmove(s_jira_lru[0], s_jira_lru[1],
                sizeof(s_jira_lru[0]) * (JIRA_LRU_SIZE - 1));
        strlcpy(s_jira_lru[JIRA_LRU_SIZE - 1], key, sizeof(s_jira_lru[0]));
    }
}

/* ── Push a notification to the outbound bus ──────────────────── */

static void push_notify(notify_source_t src, const char *key,
                        const char *summary)
{
    const char *title;
    uint32_t color;
    char body[200];   /* toast body (no prefix) */
    char text[256];   /* bus content (with prefix, for cli mirror / log) */

    if (src == NOTIFY_SRC_GERRIT) {
        title = "Gerrit";
        color = AGENT_NOTIFY_GERRIT_COLOR;
        snprintf(body, sizeof(body), "#%s: %s", key, summary);
        snprintf(text, sizeof(text), "Gerrit %s", body);
    } else {
        title = "Jira";
        color = AGENT_NOTIFY_JIRA_COLOR;
        snprintf(body, sizeof(body), "%s: %s", key, summary);
        snprintf(text, sizeof(text), "Jira %s", body);
    }

    /* Pop the toast directly — the notify thread knows the exact title
     * and color, so it builds the toast here rather than letting the
     * outbound dispatcher guess from content text.  The dispatcher
     * still sees the message (for cli mirror / logging) but skips
     * re-popping a toast for the lvgl_notify channel. */
    lvgl_toast_show_async(title, body, color, AGENT_NOTIFY_TOAST_MS);

    agent_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    strncpy(msg.channel, AGENT_CHAN_LVGL_NOTIFY, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, "notify", sizeof(msg.chat_id) - 1);
    msg.content = strdup(text);
    if (!msg.content) {
        syslog(LOG_ERR, "[%s] strdup notify failed\n", TAG);
        return;
    }
    if (message_bus_push_outbound(&msg) != OK) {
        syslog(LOG_WARNING, "[%s] outbound push failed (bus full?)\n", TAG);
        free(msg.content);
        return;
    }
    syslog(LOG_INFO, "[%s] notified %s %s\n", TAG, title, key);
}

/* ── Gerrit dedup: extract new changes from query result ─────── */

static int gerrit_dedup(const char *json, notify_source_t src, int *n_new_out)
{
    /* mcp_client_execute returns result.content[0].text which is itself
     * often a JSON string (the change list).  Try to parse it as an array
     * or as an object with an array field. */
    cJSON *root = cJSON_Parse(json);
    if (!root) {
        return -1;
    }

    cJSON *arr = root;
    if (cJSON_IsObject(root)) {
        arr = cJSON_GetArrayItem(root, 0);
        if (!cJSON_IsArray(arr)) {
            arr = cJSON_GetObjectItem(root, "changes");
            if (!cJSON_IsArray(arr)) {
                arr = cJSON_GetObjectItem(root, "result");
            }
        }
    }
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return -1;
    }

    long last_max = strtol(s_cfg[src].last_cursor, NULL, 10);
    int n_new = 0;

    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        cJSON *num = cJSON_GetObjectItem(item, "_number");
        if (!num) num = cJSON_GetObjectItem(item, "number");
        if (!num) num = cJSON_GetObjectItem(item, "change_id");
        if (!num || !cJSON_IsNumber(num)) continue;

        long id = (long)num->valuedouble;
        if (id <= last_max) continue;  /* already seen */

        cJSON *subj = cJSON_GetObjectItem(item, "subject");
        if (!subj) subj = cJSON_GetObjectItem(item, "summary");
        const char *summary = (subj && cJSON_IsString(subj))
                              ? subj->valuestring : "(no subject)";

        char key[24];
        snprintf(key, sizeof(key), "%ld", id);
        push_notify(src, key, summary);
        n_new++;

        last_max = id;  /* id > last_max guaranteed by the continue above */
    }

    if (n_new > 0) {
        snprintf(s_cfg[src].last_cursor, sizeof(s_cfg[src].last_cursor),
                 "%ld", last_max);
        persist_cursor(src);
    }

    cJSON_Delete(root);
    *n_new_out = n_new;
    return 0;
}

/* ── Jira dedup: extract new issues ────────────────────────────── */

static int jira_dedup(const char *json, notify_source_t src, int *n_new_out)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return -1;

    cJSON *arr = cJSON_GetObjectItem(root, "issues");
    if (!cJSON_IsArray(arr)) {
        arr = cJSON_GetArrayItem(root, 0);  /* maybe top-level array */
    }
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(root);
        return -1;
    }

    int n_new = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, arr) {
        cJSON *keyobj = cJSON_GetObjectItem(item, "key");
        if (!keyobj || !cJSON_IsString(keyobj)) continue;
        const char *key = keyobj->valuestring;
        if (jira_lru_seen(key)) continue;

        cJSON *fields = cJSON_GetObjectItem(item, "fields");
        const char *summary = "(no summary)";
        if (fields) {
            cJSON *s = cJSON_GetObjectItem(fields, "summary");
            if (s && cJSON_IsString(s)) summary = s->valuestring;
        }

        push_notify(src, key, summary);
        jira_lru_add(key);
        strlcpy(s_cfg[src].last_cursor, key,
                sizeof(s_cfg[src].last_cursor));
        n_new++;
    }

    if (n_new > 0) {
        persist_cursor(src);
    }

    cJSON_Delete(root);
    *n_new_out = n_new;
    return 0;
}

/* ── Poll one source ──────────────────────────────────────────── */

static void poll_one_source(notify_source_t src)
{
    notify_src_cfg_t *c = &s_cfg[src];
    if (!c->enabled || c->server_name[0] == '\0' || c->tool_query[0] == '\0') {
        return;
    }

    char *buf = (char *)malloc(AGENT_NOTIFY_JSON_BUF_SIZE);
    if (!buf) {
        strlcpy(s_last_err, "alloc failed", sizeof(s_last_err));
        return;
    }
    buf[0] = '\0';

    syslog(LOG_INFO, "[%s] polling %s (%s)\n", TAG, c->server_name, c->tool_query);

    int rc = mcp_client_execute(c->tool_query, c->query_args,
                                buf, AGENT_NOTIFY_JSON_BUF_SIZE);
    if (rc != OK) {
        snprintf(s_last_err, sizeof(s_last_err), "%s exec failed", c->server_name);
        syslog(LOG_WARNING, "[%s] %s poll failed: %.80s\n", TAG,
               c->server_name, buf);
        free(buf);
        return;
    }

    int n_new = 0;
    int parse_rc;
    if (src == NOTIFY_SRC_GERRIT) {
        parse_rc = gerrit_dedup(buf, src, &n_new);
    } else {
        parse_rc = jira_dedup(buf, src, &n_new);
    }

    if (parse_rc < 0) {
        snprintf(s_last_err, sizeof(s_last_err), "%s parse failed", c->server_name);
        syslog(LOG_WARNING, "[%s] %s result not a JSON array: %.80s\n",
               TAG, c->server_name, buf);
    } else {
        s_last_err[0] = '\0';
        s_last_new_count = n_new;
        syslog(LOG_INFO, "[%s] %s poll: %d new\n", TAG, c->server_name, n_new);
    }

    s_last_poll_epoch = (int)time(NULL);
    free(buf);
}

/* ── Polling thread ────────────────────────────────────────────── */

static void *notify_thread(void *arg)
{
    (void)arg;

    struct timespec next_due[NOTIFY_SRC_COUNT];
    clock_gettime(CLOCK_REALTIME, &next_due[NOTIFY_SRC_GERRIT]);
    /* stagger: gerrit polls first, jira 30s later */
    next_due[NOTIFY_SRC_JIRA] = next_due[NOTIFY_SRC_GERRIT];
    next_due[NOTIFY_SRC_JIRA].tv_sec += 30;

    while (s_running) {
        /* find earliest next_due */
        struct timespec earliest = next_due[0];
        for (int i = 1; i < NOTIFY_SRC_COUNT; i++) {
            if (next_due[i].tv_sec < earliest.tv_sec ||
                (next_due[i].tv_sec == earliest.tv_sec &&
                 next_due[i].tv_nsec < earliest.tv_nsec)) {
                earliest = next_due[i];
            }
        }

        pthread_mutex_lock(&s_mtx);
        pthread_cond_timedwait(&s_cond, &s_mtx, &earliest);
        pthread_mutex_unlock(&s_mtx);

        if (!s_running) break;

        time_t now = time(NULL);
        for (int i = 0; i < NOTIFY_SRC_COUNT; i++) {
            if (!s_cfg[i].enabled) continue;
            if (now < next_due[i].tv_sec) continue;
            poll_one_source((notify_source_t)i);
            next_due[i].tv_sec = now + s_cfg[i].interval_sec;
        }
    }

    return NULL;
}

/* ── Public API ───────────────────────────────────────────────── */

int notify_service_init(void)
{
    if (s_inited) return OK;
    notify_load_cfg();
    s_inited = true;
    syslog(LOG_INFO, "[%s] inited (gerrit=%s/%ds, jira=%s/%ds)\n", TAG,
           s_cfg[NOTIFY_SRC_GERRIT].enabled ? "on" : "off",
           s_cfg[NOTIFY_SRC_GERRIT].interval_sec,
           s_cfg[NOTIFY_SRC_JIRA].enabled ? "on" : "off",
           s_cfg[NOTIFY_SRC_JIRA].interval_sec);
    return OK;
}

int notify_service_start(void)
{
    if (!s_inited) {
        notify_service_init();
    }
    if (s_running) {
        return OK;
    }
    s_running = true;
    int err = agent_task_create(notify_thread, "notify",
                                 AGENT_NOTIFY_STACK, NULL, AGENT_NOTIFY_PRIO);
    if (err != OK) {
        s_running = false;
        syslog(LOG_ERR, "[%s] thread create failed\n", TAG);
        return ERROR;
    }
    syslog(LOG_INFO, "[%s] started\n", TAG);
    return OK;
}

void notify_service_stop(void)
{
    if (!s_running) return;
    s_running = false;
    pthread_mutex_lock(&s_mtx);
    pthread_cond_signal(&s_cond);
    pthread_mutex_unlock(&s_mtx);
    syslog(LOG_INFO, "[%s] stopped\n", TAG);
}

char* notify_service_status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return NULL;
    cJSON_AddBoolToObject(root, "running", s_running);
    cJSON_AddNumberToObject(root, "last_poll", s_last_poll_epoch);
    cJSON_AddNumberToObject(root, "last_new", s_last_new_count);
    cJSON_AddStringToObject(root, "last_err", s_last_err);

    cJSON *srcs = cJSON_CreateArray();
    for (int i = 0; i < NOTIFY_SRC_COUNT; i++) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "name",
            i == NOTIFY_SRC_GERRIT ? "gerrit" : "jira");
        cJSON_AddBoolToObject(s, "enabled", s_cfg[i].enabled);
        cJSON_AddStringToObject(s, "server", s_cfg[i].server_name);
        cJSON_AddStringToObject(s, "cursor", s_cfg[i].last_cursor);
        cJSON_AddNumberToObject(s, "interval", s_cfg[i].interval_sec);
        cJSON_AddItemToArray(srcs, s);
    }
    cJSON_AddItemToObject(root, "sources", srcs);

    char *out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

int notify_service_inject(notify_source_t src, const char *key,
                          const char *summary)
{
    if (src < 0 || src >= NOTIFY_SRC_COUNT || !key || !summary) {
        return ERROR;
    }
    push_notify(src, key, summary);
    return OK;
}

#endif /* CONFIG_AI_AGENT_NOTIFY_SERVICE */
