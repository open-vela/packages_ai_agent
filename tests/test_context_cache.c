/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "core/context_builder.h"

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int block_refresh, inside_refresh, release_refresh;

char *tool_registry_get_tools_json(void) { return strdup("[]"); }
size_t skill_loader_build_summary(char *buf, size_t cap)
{ return (size_t)snprintf(buf, cap, "voice skill"); }
int memory_read_long_term(char *buf, size_t cap)
{
    pthread_mutex_lock(&gate);
    if (block_refresh) {
        inside_refresh = 1;
        pthread_cond_broadcast(&changed);
        while (!release_refresh) pthread_cond_wait(&changed, &gate);
    }
    pthread_mutex_unlock(&gate);
    snprintf(buf, cap, "complete memory snapshot");
    return 0;
}
int memory_read_recent(char *buf, size_t cap, int days)
{ (void)days; snprintf(buf, cap, "daily snapshot"); return 0; }
static unsigned long long ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
static void *release_later(void *arg)
{
    (void)arg;
    usleep(300000);
    pthread_mutex_lock(&gate);
    release_refresh = 1;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&gate);
    return NULL;
}
int main(void)
{
    char prompt[8192];
    assert(context_build_system_prompt(prompt, sizeof(prompt)) == 0);
    assert(strstr(prompt, "complete memory snapshot"));
    pthread_mutex_lock(&gate);
    block_refresh = 1;
    struct timespec until;
    clock_gettime(CLOCK_REALTIME, &until);
    until.tv_sec += 2;
    while (!inside_refresh)
        assert(!pthread_cond_timedwait(&changed, &gate, &until));
    pthread_mutex_unlock(&gate);
    pthread_t timer;
    assert(!pthread_create(&timer, NULL, release_later, NULL));
    unsigned long long start = ms();
    assert(context_build_system_prompt(prompt, sizeof(prompt)) == 0);
    unsigned long long elapsed = ms() - start;
    assert(elapsed < 100); /* The slow file refresh must not hold publication. */
    assert(strstr(prompt, "complete memory snapshot"));
    assert(!pthread_join(timer, NULL));
    printf("test_context_cache: OK (query used complete prior snapshot during blocked refresh, %llu ms)\n", elapsed);
    return 0;
}
