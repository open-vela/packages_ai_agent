/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <assert.h>
#include <stdio.h>
#include <string.h>

/*
 * register_tool() is intentionally static.  Include the translation unit so
 * this test can exercise the registration filter without calling the full
 * registry initializer and every hardware tool behind it.  The Makefile links
 * with --gc-sections so the unused initializer and executors are discarded.
 */

#include "../src/tools/tool_registry.c"

int claw_config_get(const char *key, char *buf, size_t buf_size)
{
    (void)key;
    (void)buf;
    (void)buf_size;
    return ERROR;
}

int claw_config_set(const char *key, const char *value)
{
    (void)key;
    (void)value;
    return OK;
}

static int dummy_execute(const char *input_json, char *output,
                         size_t output_size)
{
    (void)input_json;
    (void)output;
    (void)output_size;
    return OK;
}

static void expect_registered(const char *name, bool registered)
{
    agent_tool_t tool = {
        .name = name,
        .description = "test",
        .input_schema_json = TOOL_NO_PARAMS,
        .execute = dummy_execute,
    };
    int before = s_tool_count;

    register_tool(&tool);
    assert((s_tool_count == before + 1) == registered);
    if (registered)
        assert(strcmp(s_tools[s_tool_count - 1].name, name) == 0);
}

int main(void)
{
    static const char *const allowed[] = {
        "read_file", "write_file", "get_current_time", "run_shell",
        "vg_point_read", "vg_run_report", NULL
    };
    static const char *const denied[] = {
        "edit_file", "list_dir", "cron_add", "web_search", "fetch_url",
        "analyze_image", "music_play", "vibrate", NULL
    };
    int i;

    s_tool_count = 0;

    for (i = 0; allowed[i] != NULL; i++)
        expect_registered(allowed[i], true);
    for (i = 0; denied[i] != NULL; i++)
        expect_registered(denied[i], false);

    assert(s_tool_count == 6);
    puts("test_tool_registry_vg_scope: OK (unrelated builtins are not registered)");
    return 0;
}
