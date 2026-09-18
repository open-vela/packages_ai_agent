/*
 * Host regression test for context_build_system_prompt().
 *
 * 链接真实的 context_builder.c，桩掉 tool_registry_get_tools_json() 与两个
 * memory 读函数。桩故意把缓冲区写到接近写满，把提示词构建路径上
 * `off += snprintf(...)` 不封顶后 `size - off - 1` 下溢的条件复现出来。
 *
 * 用法：
 *   gcc -O2 -g -Wall -Wextra -fstack-protector-all \
 *       -I tests/stub -I include -I src -I <cJSON 目录> \
 *       -DCONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR='"/tmp/vgtest"' \
 *       tests/test_context_prompt.c src/core/context_builder.c \
 *       src/tools/skill_loader.c tests/tool_registry_stub.c \
 *       <cJSON.c> -o /tmp/test_context_prompt
 *   /tmp/test_context_prompt
 *
 * 退出码非 0 表示至少一个用例失败。
 */

#include "core/context_builder.h"
#include "core/memory_store.h"
#include "tools/tool_registry.h"
#include "tools/skill_loader.h"
#include "agent_config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define CANARY_BYTE 0xA5
#define PAD 4096
#define MAX_BUF 16384

static unsigned char g_mem[PAD + MAX_BUF + PAD];
static unsigned char *g_buf;

/* ── 桩：工具列表 ───────────────────────────────────────────── */

char *tool_registry_get_tools_json(void)
{
    static const char *names[] = {
        "get_current_time", "web_search",     "read_file",
        "write_file",       "edit_file",      "list_dir",
        "get_weather",      "cron_add",       "cron_list",
        "cron_remove",      "fetch_url",      "shell_exec",
        "system_info",      "health_check",   "vision_analyze",
        "feishu_doc_read",  "feishu_chat_send", "mqtt_publish",
        "mqtt_subscribe",   "media_play",     "media_stop",
        "control_gpio",     "proxy_quickapp", "news_search",
        "translate",        "node_query",     "node_list",
        "memory_read",      "memory_write",   "skill_sync",
    };
    const size_t count = sizeof(names) / sizeof(names[0]);
    size_t cap = 4096;
    char *json = malloc(cap);
    if (!json) {
        return NULL;
    }

    size_t off = 0;
    off += (size_t)snprintf(json + off, cap - off, "[");
    for (size_t i = 0; i < count; i++) {
        off += (size_t)snprintf(json + off, cap - off, "%s{\"name\":\"%s\"}",
                                i ? "," : "", names[i]);
    }
    off += (size_t)snprintf(json + off, cap - off, "]");
    return json;
}

/* ── 桩：记忆读取 ───────────────────────────────────────────── */

/* 按真实语义把缓冲区填到 size - 1 字节，让 off 在调用后被推到 size 附近。
 * clamp 到 8192 只是防止改前的 avail 下溢成极大值时真的写爆进程。 */
int memory_read_long_term(char *buf, size_t size)
{
    if (size == 0) {
        return ERROR;
    }
    size_t n = size - 1;
    if (n > 8192) {
        n = 8192;
    }
    memset(buf, 'L', n);
    buf[n] = '\0';
    return OK;
}

int memory_read_recent(char *buf, size_t size, int days)
{
    (void)days;
    if (size == 0) {
        return ERROR;
    }
    size_t n = size - 1;
    if (n > 512) {
        n = 512;
    }
    memset(buf, 'R', n);
    buf[n] = '\0';
    return OK;
}

/* ── 测试框架 ───────────────────────────────────────────────── */

static int canary_region_ok(size_t start, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (g_mem[start + i] != CANARY_BYTE) {
            return 0;
        }
    }
    return 1;
}

static int run_case(size_t cap)
{
    memset(g_mem, CANARY_BYTE, sizeof(g_mem));

    int rc = context_build_system_prompt((char *)g_buf, cap);

    int front_ok = canary_region_ok(0, PAD);
    int rear_ok = canary_region_ok(PAD + cap, PAD);

    size_t len = (cap == 0) ? 0 : strlen((char *)g_buf);
    int len_ok = (cap == 0) ? 1 : (len < cap);
    int rc_ok = (rc == OK);

    int ok = front_ok && rear_ok && len_ok && rc_ok;

    printf("[context_prompt] cap=%-5zu len=%-6zu rc=%d front=%-3s rear=%-3s "
           "len=%-3s -> %s\n",
           cap, len, rc, front_ok ? "ok" : "BAD", rear_ok ? "ok" : "BAD",
           len_ok ? "ok" : "BAD", ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}

static void setup(void)
{
    mkdir(AGENT_DATA_DIR, 0755);
    mkdir(AGENT_CONFIG_DIR, 0755);
    mkdir(AGENT_MEMORY_DIR, 0755);
    mkdir(AGENT_SKILLS_DIR, 0755);

    FILE *f = fopen(AGENT_SOUL_FILE, "w");
    if (f) {
        fputs("# Personality\n\n我是测试人格，用于 host 用例。\n", f);
        fclose(f);
    }
    f = fopen(AGENT_USER_FILE, "w");
    if (f) {
        fputs("# User\n\n测试用户。\n", f);
        fclose(f);
    }
}

int main(void)
{
    setup();
    g_buf = g_mem + PAD;

    static const size_t caps[] = { 4096, 2048, 1024, 512, 64, 1, 0 };
    int failed = 0;

    printf("=== context_build_system_prompt canary test (dir=%s) ===\n",
           AGENT_DATA_DIR);
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        failed += run_case(caps[i]);
    }

    printf("=== %s ===\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
