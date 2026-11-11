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
 * notify_service.c — Gerrit/Jira polling → LVGL notification page.
 *
 * Thread model: a single detached notify_thread, cond_timedwait-driven,
 * each source keeping its own next-due timestamp.  Polling is a sync
 * mcp_client_execute() call on this thread (never on agent_loop).
 *
 * Dedup: Gerrit polls with a last-updated window (-age:) and keeps a small
 * in-memory LRU of the change numbers it reported, so what gets notified is
 * "updated since the previous poll" rather than "id larger than everything
 * seen".  The id-cursor version could never report a new patch set on an
 * existing change — the normal upload path — and replayed the whole
 * open-change backlog after a re-flash.  Jira keys are "PROJ-123"
 * (non-numeric) → store a last-seen key + a small in-memory LRU.
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
#include "ui/lvgl_ui_channel.h"

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
    char tool_query[64];   /* "gerrit.query_changes" / "jira.jira_search" */
    char query_args[256];   /* JSON args string */
    int interval_sec;
    char last_cursor[32];  /* Gerrit: max change num; Jira: last seen key */
} notify_src_cfg_t;

static notify_src_cfg_t s_cfg[NOTIFY_SRC_COUNT];

/* Jira in-memory LRU of recently reported keys (not persisted). */
#define JIRA_LRU_SIZE 64
static char s_jira_lru[JIRA_LRU_SIZE][24];
static int s_jira_lru_n = 0;

/* Gerrit in-memory LRU of recently reported change numbers (not persisted).
 * The poll query is already bounded to "updated in the last few minutes";
 * the LRU only keeps the overlapping windows of consecutive polls from
 * reporting the same change twice. */
#define GERRIT_LRU_SIZE 64
static long s_gerrit_lru[GERRIT_LRU_SIZE];
static int s_gerrit_lru_n = 0;

static bool gerrit_lru_seen(long id)
{
    for (int i = 0; i < s_gerrit_lru_n; i++) {
        if (s_gerrit_lru[i] == id) {
            return true;
        }
    }
    return false;
}

static void gerrit_lru_add(long id)
{
    if (s_gerrit_lru_n < GERRIT_LRU_SIZE) {
        s_gerrit_lru[s_gerrit_lru_n++] = id;
    } else {
        /* ring: drop the oldest */
        memmove(s_gerrit_lru, s_gerrit_lru + 1,
                sizeof(s_gerrit_lru[0]) * (GERRIT_LRU_SIZE - 1));
        s_gerrit_lru[GERRIT_LRU_SIZE - 1] = id;
    }
}

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
    /* Only the user's own changes.  The plain `status:open` query reported
     * every open change in the project — a poll could surface a dozen
     * unrelated subjects that kept replacing each other on the notice page.
     * `owner:` covers what we push, `reviewer:` covers review requests. */
    cfg_load_str(NOTIFY_SRC_GERRIT, "query_args", g->query_args,
                 sizeof(g->query_args),
                 "{\"query\":\"status:open (owner:zhangyi101@xiaomi.com OR "
                 "reviewer:zhangyi101@xiaomi.com)\",\"limit\":50}");
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
                 sizeof(j->tool_query), "jira.jira_search");
    {
        /* Default JQL: the last hour of updates, as a RELATIVE date.
         *
         * Relative dates are evaluated by Jira at query time, so the window
         * slides with every poll.  The previous absolute timestamp was built
         * once at boot, which froze the window: anything updated after boot
         * could never match, so the Jira leg could only ever report pre-boot
         * history.
         *
         * A relative date also sidesteps the timezone entirely.  This image
         * has no CONFIG_LIBC_LOCALTIME (localtime_r() is a UTC alias), and a
         * UTC-formatted "YYYY-MM-DD HH:MM" is read by Jira as local time
         * (UTC+8), widening the window by 8h (measured: 595 issues).
         *
         * limit=10 keeps one poll at ~16KB of HTTP body / ~7.3KB of payload
         * (measured) — inside MCP_RESP_BUF_SIZE (64KB) and
         * AGENT_NOTIFY_JSON_BUF_SIZE (16KB) respectively. */
        /* …and only the user's own issues.  Without the assignee/reporter/
         * watcher filter every updated issue in every project the token can
         * see matched, so the notice page reported strangers' tickets. */
        snprintf(j->query_args, sizeof(j->query_args),
                 "{\"jql\":\"(assignee = \\\"zhangyi101@xiaomi.com\\\" OR "
                 "reporter = \\\"zhangyi101@xiaomi.com\\\" OR "
                 "watcher = \\\"zhangyi101@xiaomi.com\\\") AND "
                 "updated >= -1h ORDER BY updated DESC\","
                 "\"fields\":\"summary,status,priority,assignee\",\"limit\":10}");
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
    char body[512];  /* toast body (no prefix) */
    char text[576];  /* bus content (with prefix, for cli mirror / log) */

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

    /* Notification page, not a toast: the round panel is too small for a top
     * banner (the body was clipped).  The page holds a fixed 5 s window
     * (LVGL_UI_NOTICE_MS), any user key exits early, and the approval page
     * outranks it. */
    lvgl_ui_channel_show_notice(title, body, color);

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

/* Text fallback: the onedev gateway's query_changes returns a plain text
 * summary whose lines look like "- 10365285: subject text".  Scan one
 * buffer line by line and notify for ids the LRU has not reported yet;
 * the query's -age: window is what makes a line "new" (a new patch set on
 * an existing change shows up with its old, smaller number). */
static void gerrit_scan_text(const char *text, notify_source_t src,
                             long *last_max, int *n_new)
{
    const char *p = text;

    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);

        /* "- <digits>: subject" */
        if (len > 4 && p[0] == '-' && p[1] == ' ') {
            char *end = NULL;
            long id = strtol(p + 2, &end, 10);
            if (end && end != p + 2 && *end == ':' && end[1] == ' '
                && id > 0 && !gerrit_lru_seen(id)) {
                const char *subj = end + 2;
                size_t slen = len - (size_t)(subj - p);
                char key[24];
                char summary[256];

                snprintf(key, sizeof(key), "%ld", id);
                snprintf(summary, sizeof(summary), "%.*s",
                         (int)(slen < sizeof(summary) - 1
                               ? slen : sizeof(summary) - 1), subj);
                /* Mark seen even when the per-poll cap skips the push, so
                 * a burst cannot turn into a flood on later polls. */
                gerrit_lru_add(id);
                if (*n_new < AGENT_NOTIFY_MAX_NEW) {
                    push_notify(src, key, summary);
                    (*n_new)++;
                }
                if (id > *last_max) {
                    *last_max = id;
                }
            }
        }
        p = eol ? eol + 1 : NULL;
    }
}

static int gerrit_dedup(const char *json, notify_source_t src, int *n_new_out)
{
    /* mcp_client_execute returns result.content[0].text.  Two shapes seen:
     *  1. structured JSON (array of change objects with _number/subject),
     *  2. the onedev gateway's text summary wrapped as
     *     [{"type":"text","text":"- 123: ...\n- 124: ..."}].
     * Try the structured path first; if no item carried a numeric id,
     * fall back to scanning the text payloads. */
    long last_max = strtol(s_cfg[src].last_cursor, NULL, 10);
    int n_new = 0;

    cJSON *root = cJSON_Parse(json);
    if (root) {
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

        int n_struct = 0;
        if (cJSON_IsArray(arr)) {
            cJSON *item;
            cJSON_ArrayForEach(item, arr) {
                cJSON *num = cJSON_GetObjectItem(item, "_number");
                if (!num) num = cJSON_GetObjectItem(item, "number");
                if (!num) num = cJSON_GetObjectItem(item, "change_id");
                if (!num || !cJSON_IsNumber(num)) continue;
                n_struct++;

                long id = (long)num->valuedouble;
                if (id <= 0 || gerrit_lru_seen(id)) continue;

                cJSON *subj = cJSON_GetObjectItem(item, "subject");
                if (!subj) subj = cJSON_GetObjectItem(item, "summary");
                const char *summary = (subj && cJSON_IsString(subj))
                                      ? subj->valuestring : "(no subject)";

                char key[24];
                snprintf(key, sizeof(key), "%ld", id);
                /* Mark seen even when the per-poll cap skips the push, so
                 * a burst cannot turn into a flood on later polls. */
                gerrit_lru_add(id);
                if (n_new < AGENT_NOTIFY_MAX_NEW) {
                    push_notify(src, key, summary);
                    n_new++;
                }

                if (id > last_max) {
                    last_max = id;  /* status display only */
                }
            }
        }

        /* No structured ids found -> the array is a text-content wrapper;
         * scan each item's "text" field for "- <id>: <subject>" lines. */
        if (n_struct == 0 && cJSON_IsArray(arr)) {
            cJSON *item;
            cJSON_ArrayForEach(item, arr) {
                cJSON *text = cJSON_GetObjectItem(item, "text");
                if (text && cJSON_IsString(text)) {
                    gerrit_scan_text(text->valuestring, src,
                                     &last_max, &n_new);
                }
            }
        } else if (n_struct == 0) {
            /* Bare string / other shape: try the raw buffer as text. */
            cJSON *text = (root && cJSON_IsString(root)) ? root : NULL;
            if (text) {
                gerrit_scan_text(text->valuestring, src, &last_max, &n_new);
            }
        }

        cJSON_Delete(root);
    } else {
        /* Not JSON at all — treat the whole buffer as text output. */
        gerrit_scan_text(json, src, &last_max, &n_new);
    }

    if (n_new > 0) {
        snprintf(s_cfg[src].last_cursor, sizeof(s_cfg[src].last_cursor),
                 "%ld", last_max);
        persist_cursor(src);
    }

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
        /* jira_search with a "fields" argument returns the requested fields
         * inline on each issue (no "fields" wrapper); full-issue responses
         * nest them under "fields".  Accept both. */
        cJSON *s = cJSON_GetObjectItem(item, "summary");
        if (!s && fields) {
            s = cJSON_GetObjectItem(fields, "summary");
        }
        if (s && cJSON_IsString(s)) summary = s->valuestring;

        /* Mark every issue seen (even past the cap) so a large backlog
         * is not re-scanned as new on the next poll; only the first
         * AGENT_NOTIFY_MAX_NEW issues pop a toast. */
        jira_lru_add(key);
        strlcpy(s_cfg[src].last_cursor, key,
                sizeof(s_cfg[src].last_cursor));
        if (n_new < AGENT_NOTIFY_MAX_NEW) {
            push_notify(src, key, summary);
            n_new++;
        }
    }

    if (n_new > 0) {
        persist_cursor(src);
    }

    cJSON_Delete(root);
    *n_new_out = n_new;
    return 0;
}

/* ── Gerrit poll query: configured query + last-updated window ─── */

#define GERRIT_DEFAULT_QUERY \
    "status:open (owner:zhangyi101@xiaomi.com OR " \
    "reviewer:zhangyi101@xiaomi.com)"

/* Append `-age:<N>min` to the configured query so the server returns only
 * changes whose last update falls inside the window; that is what makes a
 * new patch set on an existing change a notification event.  `-age:` takes
 * a relative age evaluated by Gerrit itself, so the device clock and this
 * image's missing locale TZ cannot skew the window.  Twice the poll
 * interval (+1 min) tolerates a slow or skipped poll without dropping an
 * update; the id LRU absorbs the overlap. */
static void gerrit_poll_args(char *out, size_t out_sz)
{
    notify_src_cfg_t *c = &s_cfg[NOTIFY_SRC_GERRIT];
    int interval = c->interval_sec > 0 ? c->interval_sec
                                       : AGENT_NOTIFY_INTERVAL_SEC;
    int mins = (interval * 2 + 59) / 60 + 1;
    char qbuf[224];

    cJSON *args = cJSON_Parse(c->query_args);
    if (args == NULL) {
        args = cJSON_CreateObject();
    }
    if (args == NULL) {
        out[0] = '\0';
        return;
    }

    cJSON *q = cJSON_GetObjectItem(args, "query");
    const char *base = (q && cJSON_IsString(q)) ? q->valuestring
                                                : GERRIT_DEFAULT_QUERY;
    snprintf(qbuf, sizeof(qbuf), "%s -age:%dmin", base, mins);

    cJSON_DeleteItemFromObject(args, "query");
    cJSON_AddStringToObject(args, "query", qbuf);
    if (cJSON_GetObjectItem(args, "limit") == NULL) {
        cJSON_AddNumberToObject(args, "limit", 50);
    }

    char *j = cJSON_PrintUnformatted(args);
    if (j != NULL) {
        strlcpy(out, j, out_sz);
        free(j);
    } else {
        out[0] = '\0';
    }
    cJSON_Delete(args);
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

    /* Gerrit polls through a freshly built time-windowed query; every other
     * source uses its configured args verbatim. */
    char gerrit_args[320];
    const char *args = c->query_args;
    if (src == NOTIFY_SRC_GERRIT) {
        gerrit_poll_args(gerrit_args, sizeof(gerrit_args));
        if (gerrit_args[0] != '\0') {
            args = gerrit_args;
        }
    }

    syslog(LOG_INFO, "[%s] polling %s (%s)\n", TAG, c->server_name, c->tool_query);
    syslog(LOG_INFO, "[%s] %s args: %s\n", TAG, c->server_name, args);

    int rc = mcp_client_execute(c->tool_query, args,
                                buf, AGENT_NOTIFY_JSON_BUF_SIZE);
    if (rc != OK && strstr(buf, "not discovered")) {
        /* Self-heal: if the boot-time discover failed (network race or a
         * transient gateway error), the tool table stays empty and every
         * poll fails forever.  Re-discover now — the poll interval already
         * rate-limits this — and retry once. */
        syslog(LOG_WARNING, "[%s] %s tools missing, re-discovering\n",
               TAG, c->server_name);
        mcp_client_discover();
        buf[0] = '\0';
        rc = mcp_client_execute(c->tool_query, args,
                                buf, AGENT_NOTIFY_JSON_BUF_SIZE);
    }
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
