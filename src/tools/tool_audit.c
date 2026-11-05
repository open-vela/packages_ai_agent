/* SPDX-License-Identifier: Apache-2.0 */
#include "agent_compat.h"
#include "tools/tool_audit.h"
#include "tools/tool_guard.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#ifndef VG_TOOL_AUDIT_DIR
#define VG_TOOL_AUDIT_DIR "/data/velaguard/logs"
#endif
#ifndef VG_AUDIT_BEFORE_WRITE
#define VG_AUDIT_BEFORE_WRITE() ((void)0)
#endif
#define AUDIT_SLOTS 32
#define AUDIT_LINE 256
#define AUDIT_MAX_BYTES (64 * 1024)

static pthread_mutex_t audit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t audit_wake = PTHREAD_COND_INITIALIZER;
static pthread_once_t audit_once = PTHREAD_ONCE_INIT;
static pthread_t audit_tid;
static char audit_queue[AUDIT_SLOTS][AUDIT_LINE];
static unsigned audit_head, audit_tail, audit_count, audit_inflight;
static bool audit_stop, audit_running;
static uint32_t audit_submitted, audit_written, audit_lost;

static uint64_t audit_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static unsigned audit_write_batch(char (*lines)[AUDIT_LINE], unsigned n)
{
    char path[192], old[196];
    struct stat st;
    FILE *fp;
    unsigned written = 0;
    size_t size = 0;
    VG_AUDIT_BEFORE_WRITE();
    snprintf(path, sizeof(path), "%s/agent_tools.log", VG_TOOL_AUDIT_DIR);
    snprintf(old, sizeof(old), "%s.1", path);
    mkdir(VG_TOOL_AUDIT_DIR, 0755);
    if (stat(path, &st) == 0) size = (size_t)st.st_size;
    fp = fopen(path, "a");
    if (!fp) return 0;
    for (unsigned i = 0; i < n; i++) {
        size_t len = strlen(lines[i]);
        if (size && size + len > AUDIT_MAX_BYTES) {
            if (fclose(fp) != 0) return 0;
            if (unlink(old) < 0 && errno != ENOENT) return 0;
            if (rename(path, old) != 0) return 0;
            fp = fopen(path, "w");
            if (!fp) return 0;
            size = 0;
        }
        if (fwrite(lines[i], 1, len, fp) != len) break;
        size += len;
        written++;
    }
    if (fclose(fp) != 0) return 0;
    return written;
}

static void *audit_worker(void *arg)
{
    char (*batch)[AUDIT_LINE] = arg;
    pthread_mutex_lock(&audit_lock);
    for (;;) {
        while (!audit_count && !audit_stop)
            pthread_cond_wait(&audit_wake, &audit_lock);
        if (!audit_count && audit_stop) break;
        /* 合并模型工具和事实复核的记录，减少文件开关次数。 */
        pthread_mutex_unlock(&audit_lock);
        usleep(250000);
        pthread_mutex_lock(&audit_lock);
        unsigned n = audit_count;
        for (unsigned i = 0; i < n; i++) {
            memcpy(batch[i], audit_queue[audit_tail], AUDIT_LINE);
            audit_tail = (audit_tail + 1) % AUDIT_SLOTS;
        }
        audit_count = 0;
        audit_inflight = n;
        pthread_mutex_unlock(&audit_lock);
        unsigned written = audit_write_batch(batch, n);
        pthread_mutex_lock(&audit_lock);
        audit_written += written;
        audit_lost += n - written;
        audit_inflight = 0;
    }
    audit_running = false;
    pthread_mutex_unlock(&audit_lock);
    free(batch);
    return NULL;
}

static void audit_start(void)
{
    char (*batch)[AUDIT_LINE] = calloc(AUDIT_SLOTS, AUDIT_LINE);
    pthread_attr_t attr;
    int ret = ENOMEM;
    if (batch) {
        pthread_attr_init(&attr);
#ifdef __NuttX__
        pthread_attr_setstacksize(&attr, 8192);
        /* 与板端现有文件写线程使用相同调度级别，再验证存储超时。 */
        struct sched_param sp = {.sched_priority = 100};
        pthread_attr_setschedparam(&attr, &sp);
        pthread_attr_setschedpolicy(&attr, SCHED_RR);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
#else
        pthread_attr_setstacksize(&attr, 64 * 1024);
#endif
        ret = pthread_create(&audit_tid, &attr, audit_worker, batch);
        pthread_attr_destroy(&attr);
    }
    pthread_mutex_lock(&audit_lock);
    audit_running = ret == 0;
    pthread_mutex_unlock(&audit_lock);
    if (ret) {
        free(batch);
        syslog(LOG_ERR, "[tool_audit] writer unavailable rc=%d; losses are counted\n", ret);
    }
}

void tool_audit_submit(const char *name, const char *input_json, int rc)
{
    char safe[96] = {0}, tool[49] = {0}, line[AUDIT_LINE] = {0};
    struct timespec t = {0};
    if (!name) return;
    tool_guard_sanitize_log(input_json, safe, sizeof(safe));
    unsigned i;
    for (i = 0; i < sizeof(tool)-1 && name[i]; i++) {
        unsigned char c = name[i];
        tool[i] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '_') ? c : '_';
    }
    tool[i] = 0;
    clock_gettime(CLOCK_MONOTONIC, &t);
    snprintf(line, sizeof(line), "%lu.%03lu tool=%s rc=%d args=%s\n",
        (unsigned long)t.tv_sec, (unsigned long)(t.tv_nsec / 1000000), tool, rc, safe);
#ifndef CONFIG_VG_HMI
    unsigned written = audit_write_batch(&line, 1);
    pthread_mutex_lock(&audit_lock);
    audit_submitted++;
    audit_written += written;
    audit_lost += 1 - written;
    pthread_mutex_unlock(&audit_lock);
    return;
#endif
    pthread_once(&audit_once, audit_start);
    pthread_mutex_lock(&audit_lock);
    audit_submitted++;
    if (!audit_running || audit_count == AUDIT_SLOTS) {
        audit_lost++;
        if ((audit_lost & (audit_lost - 1)) == 0)
            syslog(LOG_WARNING, "[tool_audit] lost=%u (bounded queue or storage failure)\n", (unsigned)audit_lost);
    } else {
        memcpy(audit_queue[audit_head], line, sizeof(line));
        audit_head = (audit_head + 1) % AUDIT_SLOTS;
        audit_count++;
        pthread_cond_signal(&audit_wake);
    }
    pthread_mutex_unlock(&audit_lock);
}

void tool_audit_get_stats(tool_audit_stats_t *out)
{
    if (!out) return;
    pthread_mutex_lock(&audit_lock);
    *out = (tool_audit_stats_t){audit_submitted, audit_written, audit_lost,
        audit_count + audit_inflight, audit_running};
    pthread_mutex_unlock(&audit_lock);
}

int tool_audit_flush(uint32_t timeout_ms)
{
    uint64_t start = audit_ms();
    for (;;) {
        tool_audit_stats_t st;
        tool_audit_get_stats(&st);
        if (!st.pending) return st.lost ? -EIO : 0;
        if (audit_ms() - start >= timeout_ms) return -ETIMEDOUT;
        usleep(10000);
    }
}
