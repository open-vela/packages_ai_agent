/*
 * agent_logbuf.c - Ring buffer for agent logs
 */

#include "infra/agent_logbuf.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOGBUF_MAX_LINES 64
#define LOGBUF_LINE_MAX 256

static char s_buf[LOGBUF_MAX_LINES][LOGBUF_LINE_MAX];
static int s_idx = 0;
static int s_count = 0;
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

void agent_logbuf_init(void)
{
    memset(s_buf, 0, sizeof(s_buf));
    s_idx = 0;
    s_count = 0;
}

void agent_logbuf_push(const char* fmt, ...)
{
    char line[LOGBUF_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    /* Strip trailing newline */
    size_t len = strlen(line);
    if (len > 0 && line[len - 1] == '\n')
        line[len - 1] = '\0';

    pthread_mutex_lock(&s_lock);
    strncpy(s_buf[s_idx], line, LOGBUF_LINE_MAX - 1);
    s_buf[s_idx][LOGBUF_LINE_MAX - 1] = '\0';
    s_idx = (s_idx + 1) % LOGBUF_MAX_LINES;
    if (s_count < LOGBUF_MAX_LINES)
        s_count++;
    pthread_mutex_unlock(&s_lock);
}

cJSON* agent_logbuf_dump(void)
{
    cJSON* arr = cJSON_CreateArray();

    pthread_mutex_lock(&s_lock);
    int start = (s_count < LOGBUF_MAX_LINES) ? 0 : s_idx;
    for (int i = 0; i < s_count; i++) {
        int pos = (start + i) % LOGBUF_MAX_LINES;
        cJSON_AddItemToArray(arr, cJSON_CreateString(s_buf[pos]));
    }
    pthread_mutex_unlock(&s_lock);

    return arr;
}
