/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "../src/tools/tool_guard.c"

static const char *g_configured_tool;
static const char *g_configured_value = "1";

int claw_config_get(const char *key, char *buf, size_t buf_size)
{
    char enabled[GUARD_KEY_BUF];
    size_t len;
    int n;

    if (g_configured_tool == NULL)
        return ERROR;

    n = snprintf(enabled, sizeof(enabled), "tool_enabled_%s",
                 g_configured_tool);
    assert(n > 0 && (size_t)n < sizeof(enabled));

    if (strcmp(key, enabled) != 0)
        return ERROR;

    len = strlen(g_configured_value);
    if (len >= buf_size)
        return ERROR;
    memcpy(buf, g_configured_value, len + 1);
    return OK;
}

int claw_config_set(const char *key, const char *value)
{
    (void)key;
    (void)value;
    return OK;
}

int main(void)
{
    static const char *const allowed[] = {
        "read_file", "write_file", "get_current_time", "run_shell",
        "vg_point_read", "vg_run_report", NULL
    };
    static const char *const denied[] = {
        "edit_file", "list_dir", "cron_add", "cron_list", "cron_remove",
        "web_search", "news_search", "get_weather", "fetch_url",
        "analyze_image", "get_battery", "feishu_doc_read", "music_play",
        "vibrate", NULL
    };
    int i;

    for (i = 0; allowed[i] != NULL; i++)
        assert(tool_guard_is_enabled(allowed[i]));

    for (i = 0; denied[i] != NULL; i++) {
        assert(!tool_guard_is_enabled(denied[i]));
        assert(tool_guard_check(denied[i], "{}", 2) == GUARD_DENY_DISABLED);
    }

    assert(!tool_guard_is_enabled(NULL));
    assert(!tool_guard_is_enabled(""));

    /* Config can narrow the ceiling, but cannot widen it. */
    g_configured_tool = "run_shell";
    g_configured_value = "0";
    assert(!tool_guard_is_enabled("run_shell"));
    assert(tool_guard_check("run_shell", "{}", 2) == GUARD_DENY_DISABLED);

    g_configured_tool = "web_search";
    g_configured_value = "1";
    assert(!tool_guard_is_enabled("web_search"));
    assert(tool_guard_check("web_search", "{}", 2) == GUARD_DENY_DISABLED);

    puts("test_tool_guard_vg_scope: OK (hard allowlist, config can only narrow)");
    return 0;
}
