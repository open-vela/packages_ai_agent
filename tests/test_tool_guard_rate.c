/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <time.h>

#include "../src/tools/tool_guard.c"

static time_t g_now = 1000;

time_t __wrap_time(time_t *out)
{
    if (out != NULL)
        *out = g_now;
    return g_now;
}

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

struct burst_arg {
    int allowed;
    int denied;
};

static void *guard_burst(void *arg)
{
    struct burst_arg *result = arg;
    tool_guard_result_t ret =
        tool_guard_check("run_shell", "{}", 2);

    if (ret == GUARD_ALLOW)
        result->allowed++;
    else if (ret == GUARD_DENY_RATE_LIMIT)
        result->denied++;
    else
        assert(false);

    return NULL;
}

int main(void)
{
    pthread_t threads[24];
    struct burst_arg results[24];
    int allowed = 0;
    int denied = 0;
    int i;

    assert(tool_guard_init() == OK);

    /* The check reserves each allowed sensitive call.  No execution and no
     * completion callback is needed for failures to consume the budget. */
    for (i = 0; i < AGENT_TOOL_RATE_LIMIT_MAX_CALLS; i++)
        assert(tool_guard_check("run_shell", "{}", 2) == GUARD_ALLOW);
    assert(tool_guard_check("run_shell", "{}", 2) == GUARD_DENY_RATE_LIMIT);

    /* After the whole window expires, the next ten calls are admitted. */
    g_now += AGENT_TOOL_RATE_LIMIT_WINDOW_SEC + 1;
    for (i = 0; i < AGENT_TOOL_RATE_LIMIT_MAX_CALLS; i++)
        assert(tool_guard_check("run_shell", "{}", 2) == GUARD_ALLOW);
    assert(tool_guard_check("run_shell", "{}", 2) == GUARD_DENY_RATE_LIMIT);

    /* A fresh window with concurrent callers must admit exactly the budget,
     * not one call per racing thread. */
    g_now += AGENT_TOOL_RATE_LIMIT_WINDOW_SEC + 1;
    assert(tool_guard_init() == OK);
    memset(results, 0, sizeof(results));
    for (i = 0; i < 24; i++)
        assert(pthread_create(&threads[i], NULL, guard_burst,
                              &results[i]) == 0);
    for (i = 0; i < 24; i++)
        assert(pthread_join(threads[i], NULL) == 0);

    for (i = 0; i < 24; i++) {
        allowed += results[i].allowed;
        denied += results[i].denied;
    }
    assert(allowed == AGENT_TOOL_RATE_LIMIT_MAX_CALLS);
    assert(denied == 24 - AGENT_TOOL_RATE_LIMIT_MAX_CALLS);

    puts("test_tool_guard_rate: OK (failed calls count, check/reserve is atomic)");
    return 0;
}
