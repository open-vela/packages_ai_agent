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

#include "tools/tool_guard.h"
#include "infra/config_store.h"
#include "agent_compat.h"
#include "agent_config.h"

#include "cJSON.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static const char *TAG = "guard";

/* ── Sensitive tool table (const → Flash, not RAM) ─────────── */

typedef struct {
    const char *name;
    tool_security_level_t level;
} tool_sec_entry_t;

static const tool_sec_entry_t s_sec_table[] = {
    { "run_shell",  TOOL_SEC_SENSITIVE },
    { "write_file", TOOL_SEC_SENSITIVE },
    { "edit_file",  TOOL_SEC_SENSITIVE },
    { "vibrate",    TOOL_SEC_MODERATE  },
    { "cron_add",   TOOL_SEC_MODERATE  },
};

#define SEC_TABLE_SIZE (sizeof(s_sec_table) / sizeof(s_sec_table[0]))

/* ── Rate limiter state ────────────────────────────────────── */

#define RATE_SLOTS AGENT_TOOL_RATE_LIMIT_MAX_CALLS

typedef struct {
    time_t timestamps[RATE_SLOTS];
    int head;
    int count;
} rate_state_t;

/* One rate state per sensitive tool (run_shell, write_file, edit_file) */
#define SENSITIVE_TOOL_COUNT 3

static const char *s_sensitive_names[SENSITIVE_TOOL_COUNT] = {
    "run_shell", "write_file", "edit_file"
};

static rate_state_t s_rate[SENSITIVE_TOOL_COUNT];
static pthread_mutex_t s_guard_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ── Config store key prefix for tool enable/disable ───────── */

#define GUARD_KEY_PREFIX "tool_enabled_"
#define GUARD_KEY_BUF    64

#ifdef CONFIG_VG_HMI
/* VelaGuard exposes only the tools used by its Skills and board-driven
 * rounds.  This is a hard ceiling: runtime config may disable an allowed
 * tool, but it cannot opt an unrelated builtin back in. */

static bool vg_product_tool_allowed(const char *tool_name)
{
    static const char *const allowed[] = {
        "read_file",
        "write_file",
        "get_current_time",
        "run_shell",
        "vg_point_read",
        "vg_run_report",
        "vg_anomaly_read",
        "vg_alarm_recovery",
        NULL
    };

    for (int i = 0; allowed[i] != NULL; i++) {
        if (strcmp(tool_name, allowed[i]) == 0) {
            return true;
        }
    }

    return false;
}
#endif

/* ── Sensitive field names to redact in logs ───────────────── */

static const char *s_redact_keys[] = {
    "api_key", "token", "password", "secret",
    "app_secret", "api_secret"
};

#define REDACT_KEY_COUNT (sizeof(s_redact_keys) / sizeof(s_redact_keys[0]))

/* ── Internal helpers ──────────────────────────────────────── */

static tool_security_level_t get_security_level(const char *name)
{
    for (size_t i = 0; i < SEC_TABLE_SIZE; i++) {
        if (strcmp(s_sec_table[i].name, name) == 0) {
            return s_sec_table[i].level;
        }
    }
    return TOOL_SEC_SAFE;
}

static int find_sensitive_index(const char *name)
{
    for (int i = 0; i < SENSITIVE_TOOL_COUNT; i++) {
        if (strcmp(s_sensitive_names[i], name) == 0) {
            return i;
        }
    }
    return -1;
}

static void record_rate(int idx);

static bool check_and_reserve_rate(int idx)
{
    rate_state_t *rs = &s_rate[idx];
    time_t now = time(NULL);
    time_t window = (time_t)AGENT_TOOL_RATE_LIMIT_WINDOW_SEC;

    /* Expire old entries */
    while (rs->count > 0) {
        int oldest = (rs->head - rs->count + RATE_SLOTS) % RATE_SLOTS;
        if (now - rs->timestamps[oldest] > window) {
            rs->count--;
        } else {
            break;
        }
    }

    if (rs->count >= AGENT_TOOL_RATE_LIMIT_MAX_CALLS)
        return false;

    record_rate(idx);
    return true;
}

static void record_rate(int idx)
{
    rate_state_t *rs = &s_rate[idx];
    rs->timestamps[rs->head] = time(NULL);
    rs->head = (rs->head + 1) % RATE_SLOTS;
    if (rs->count < RATE_SLOTS) {
        rs->count++;
    }
}

/* ── Prompt injection pattern detection ─────────────────── */

static const char *s_injection_patterns[] = {
    "ignore all previous",
    "ignore your instructions",
    "disregard your",
    "override your",
    "forget your rules",
    "reveal your prompt",
    "show me your system",
    "print your instructions",
    "you are now",
    "act as if",
    "pretend you are",
    "new persona",
    NULL
};

static bool check_injection_pattern(const char *text)
{
    if (!text) {
        return false;
    }

    /* Case-insensitive substring search for known patterns */
    for (int i = 0; s_injection_patterns[i]; i++) {
        if (strcasestr(text, s_injection_patterns[i]) != NULL) {
            syslog(LOG_WARNING,
                "[%s] Injection pattern detected: %.40s\n",
                TAG, s_injection_patterns[i]);
            return true;
        }
    }
    return false;
}

/* ── Public API ────────────────────────────────────────────── */

int tool_guard_init(void)
{
    pthread_mutex_lock(&s_guard_mtx);
    memset(s_rate, 0, sizeof(s_rate));
    pthread_mutex_unlock(&s_guard_mtx);

    syslog(LOG_INFO, "[%s] Tool guard initialized "
           "(max_input=%d, rate_window=%ds, rate_max=%d)\n",
           TAG, AGENT_TOOL_MAX_INPUT_LEN,
           AGENT_TOOL_RATE_LIMIT_WINDOW_SEC,
           AGENT_TOOL_RATE_LIMIT_MAX_CALLS);
    return OK;
}

void tool_guard_cleanup(void)
{
    /* Nothing to free — all static */
}

tool_guard_result_t tool_guard_check(const char *tool_name,
                                     const char *input_json,
                                     size_t input_len)
{
    if (!tool_name) {
        return GUARD_DENY_INPUT_INVALID;
    }

    /* 1. Check if tool is disabled via config store */
    if (!tool_guard_is_enabled(tool_name)) {
        syslog(LOG_WARNING, "[%s] Tool '%s' is disabled\n",
               TAG, tool_name);
        return GUARD_DENY_DISABLED;
    }

    /* 2. Input size check */
    if (input_len > (size_t)AGENT_TOOL_MAX_INPUT_LEN) {
        syslog(LOG_WARNING, "[%s] Tool '%s' input too large: "
               "%zu > %d\n", TAG, tool_name,
               input_len, AGENT_TOOL_MAX_INPUT_LEN);
        return GUARD_DENY_INPUT_SIZE;
    }

    /* 3. Rate limit for sensitive tools */
    tool_security_level_t level = get_security_level(tool_name);
    if (level == TOOL_SEC_SENSITIVE) {
        int idx = find_sensitive_index(tool_name);
        if (idx >= 0) {
            pthread_mutex_lock(&s_guard_mtx);
            bool reserved = check_and_reserve_rate(idx);
            pthread_mutex_unlock(&s_guard_mtx);

            if (!reserved) {
                syslog(LOG_WARNING,
                       "[%s] Tool '%s' rate limited "
                       "(%d calls in %ds window)\n",
                       TAG, tool_name,
                       AGENT_TOOL_RATE_LIMIT_MAX_CALLS,
                       AGENT_TOOL_RATE_LIMIT_WINDOW_SEC);
                return GUARD_DENY_RATE_LIMIT;
            }
        }
    }

    return GUARD_ALLOW;
}

int tool_guard_set_enabled(const char *tool_name, bool enabled)
{
    char key[GUARD_KEY_BUF];
    int n = snprintf(key, sizeof(key), "%s%s",
                     GUARD_KEY_PREFIX, tool_name);
    if (n < 0 || (size_t)n >= sizeof(key)) {
        return ERROR;
    }
    return claw_config_set(key, enabled ? "1" : "0");
}

bool tool_guard_is_enabled(const char *tool_name)
{
    char key[GUARD_KEY_BUF];
    char val[8];

    if (tool_name == NULL || tool_name[0] == '\0') {
        return false;
    }

#ifdef CONFIG_VG_HMI
    if (!vg_product_tool_allowed(tool_name)) {
        return false;
    }
#endif

    int n = snprintf(key, sizeof(key), "%s%s",
                     GUARD_KEY_PREFIX, tool_name);
    if (n < 0 || (size_t)n >= sizeof(key)) {
        return true; /* default: enabled */
    }

    if (claw_config_get(key, val, sizeof(val)) != OK) {
        return true; /* not configured → enabled */
    }

    return strcmp(val, "0") != 0;
}

bool tool_guard_check_injection(const char *user_message)
{
    return check_injection_pattern(user_message);
}

void tool_guard_sanitize_log(const char *input_json,
                             char *safe_buf, size_t safe_size)
{
    if (!input_json || !safe_buf || safe_size == 0) {
        if (safe_buf && safe_size > 0) {
            safe_buf[0] = '\0';
        }
        return;
    }

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        strncpy(safe_buf, input_json, safe_size - 1);
        safe_buf[safe_size - 1] = '\0';
        return;
    }

    /* Redact sensitive fields in-place */
    for (size_t i = 0; i < REDACT_KEY_COUNT; i++) {
        cJSON *item = cJSON_GetObjectItem(root, s_redact_keys[i]);
        if (item && cJSON_IsString(item)) {
            cJSON_SetValuestring(item, "***REDACTED***");
        }
    }

    char *sanitized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (sanitized) {
        strncpy(safe_buf, sanitized, safe_size - 1);
        safe_buf[safe_size - 1] = '\0';
        free(sanitized);
    } else {
        strncpy(safe_buf, "{}", safe_size - 1);
        safe_buf[safe_size - 1] = '\0';
    }
}
