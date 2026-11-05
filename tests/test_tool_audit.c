/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
static char audit_dir[] = "/tmp/vg-audit-XXXXXX";
static pthread_mutex_t test_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t test_ready = PTHREAD_COND_INITIALIZER;
static int blocked, released;
static void before_write(void)
{
    pthread_mutex_lock(&test_lock);
    blocked = 1;
    pthread_cond_broadcast(&test_ready);
    while (!released) pthread_cond_wait(&test_ready, &test_lock);
    pthread_mutex_unlock(&test_lock);
}
#define VG_TOOL_AUDIT_DIR audit_dir
#define VG_AUDIT_BEFORE_WRITE() before_write()
#include "../src/tools/tool_audit.c"

int main(void)
{
    assert(mkdtemp(audit_dir));
    tool_audit_submit("vg_point_read", "{\"query\":\"ups_load\",\"api_key\":\"dummy-secret-do-not-store\"}", 0);
    pthread_mutex_lock(&test_lock);
    while (!blocked) pthread_cond_wait(&test_ready, &test_lock);
    pthread_mutex_unlock(&test_lock);
    uint64_t start = audit_ms();
    for (int i = 0; i < AUDIT_SLOTS + 5; i++)
        tool_audit_submit("blocked\nforged", "{}", -2);
    assert(audit_ms() - start < 100);
    tool_audit_stats_t st;
    tool_audit_get_stats(&st);
    assert(st.pending == AUDIT_SLOTS + 1 && st.lost == 5 && st.written == 0);
    assert(tool_audit_flush(50) == -ETIMEDOUT);
    pthread_mutex_lock(&test_lock);
    released = 1;
    pthread_cond_broadcast(&test_ready);
    pthread_mutex_unlock(&test_lock);
    assert(tool_audit_flush(5000) == -EIO); /* overflow remains visible */
    tool_audit_get_stats(&st);
    assert(st.pending == 0 && st.written == AUDIT_SLOTS + 1 && st.lost == 5);
    char path[192], contents[16384];
    snprintf(path, sizeof(path), "%s/agent_tools.log", audit_dir);
    FILE *f = fopen(path, "r"); assert(f);
    size_t n = fread(contents, 1, sizeof(contents) - 1, f);
    contents[n] = 0; fclose(f);
    assert(strstr(contents, "REDACTED") && !strstr(contents, "dummy-secret"));
    assert(strstr(contents, "blocked_forged") && !strstr(contents, "\nforged"));
    char backup[196];
    snprintf(backup, sizeof(backup), "%s.1", path);
    for (int i = 0; i < 2; i++) {
        f = fopen(path, "w"); assert(f);
        assert(!fseek(f, AUDIT_MAX_BYTES - 2, SEEK_SET));
        assert(fputc('x', f) != EOF && !fclose(f));
        tool_audit_submit("rotate", "{}", 0);
        assert(tool_audit_flush(5000) == -EIO);
        struct stat rotated, current;
        assert(!stat(backup, &rotated) && !stat(path, &current));
        assert(rotated.st_size == AUDIT_MAX_BYTES - 1 && current.st_size < AUDIT_LINE);
    }
    pthread_mutex_lock(&audit_lock);
    audit_stop = true; pthread_cond_signal(&audit_wake);
    pthread_mutex_unlock(&audit_lock);
    assert(!pthread_join(audit_tid, NULL));
    unlink(path); unlink(backup); rmdir(audit_dir);
    puts("test_tool_audit: OK (blocked writer, fast enqueue, bounded overflow counted, drain, real redaction, no forged line)");
    return 0;
}
