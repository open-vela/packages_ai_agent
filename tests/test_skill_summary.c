/*
 * Host regression test for skill_loader_build_summary().
 *
 * 被测缓冲区夹在两片 0xA5 填充区之间，调用后逐字节比对两片填充区是否
 * 完好。这对应板上的故障形态：越界写落在相邻对象（这里就是填充区）上。
 *
 * 测试自己写 10 个技能文件，格式按 skill-creator.md 的约定（首行标题、
 * 次行描述），让摘要总长超过 1024 且略高于板上日志的 1086，从而同时覆盖
 * 板端 skills_buf[1024] 与 buf[1086] 这两个边界。
 *
 * 用法：
 *   gcc -O2 -g -Wall -Wextra -fstack-protector-all \
 *       -include tests/stub/host_compat.h \
 *       -I tests/stub -I include -I src \
 *       -DCONFIG_EXAMPLES_AI_AGENT_VELA_DATA_DIR='"/tmp/vgtest"' \
 *       tests/test_skill_summary.c src/tools/skill_loader.c \
 *       tests/tool_registry_stub.c -o /tmp/test_skill_summary
 *   /tmp/test_skill_summary
 *
 * 退出码非 0 表示至少一个用例失败。
 */

#include "tools/skill_loader.h"
#include "agent_config.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define CANARY_BYTE 0xA5
#define PAD 4096
#define MAX_BUF 8192

/* 静态区避免缓冲区过小时越界写真的碰到栈保护或 unmapped 页。 */
static unsigned char g_mem[PAD + MAX_BUF + PAD];

static unsigned char *g_buf;

/* ── 测试技能文件 ───────────────────────────────────────────── */

typedef struct {
    const char *name;
    const char *title;
    const char *desc;
} test_skill_t;

static const test_skill_t s_skills[] = {
    { "weather",        "Weather",        "Get current weather and forecasts."     },
    { "daily-briefing", "Daily Briefing", "Compile a personalized daily briefing." },
    { "skill-creator",  "Skill Creator",  "Create new skills for the agent."       },
    { "system-health",  "System Health",  "Check agent system status and summarize." },
    { "reminder",       "Reminder",       "Set timed reminders that notify the user." },
    { "note-taker",     "Note Taker",     "Quick notes saved to daily diary files." },
    { "translate",      "Translate",      "Translate text between languages."      },
    { "news-digest",    "News Digest",    "Search and compile news summaries."     },
    { "feishu-test",    "Feishu Test",    "Test Feishu bot capabilities end to end." },
    { "task-manager",   "Task Manager",   "Manage a TODO list with add and view."  },
};

#define NUM_TEST_SKILLS (sizeof(s_skills) / sizeof(s_skills[0]))

static void write_test_skills(void)
{
    mkdir(AGENT_DATA_DIR, 0755);
    mkdir(AGENT_SKILLS_DIR, 0755);

    for (size_t i = 0; i < NUM_TEST_SKILLS; i++) {
        char path[256];
        snprintf(path, sizeof(path), "%s%s.md",
                 AGENT_SKILLS_DIR, s_skills[i].name);

        FILE *f = fopen(path, "w");
        if (!f) {
            fprintf(stderr, "cannot write %s\n", path);
            continue;
        }
        fprintf(f, "# %s\n%s\n\n## When to use\nWhen the task matches.\n",
                s_skills[i].title, s_skills[i].desc);
        fclose(f);
    }
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

    size_t n = skill_loader_build_summary((char *)g_buf, cap);

    int front_ok = canary_region_ok(0, PAD);
    int rear_ok = canary_region_ok(PAD + cap, PAD);

    int len_ok;
    int nul_ok = 1;

    if (cap == 0) {
        len_ok = (n == 0);
        nul_ok = 1; /* size == 0 时函数不写缓冲区 */
    } else {
        len_ok = (n < cap);
        nul_ok = (strlen((char *)g_buf) == n);
    }

    int ok = front_ok && rear_ok && len_ok && nul_ok;

    printf("[skill_summary] cap=%-5zu n=%-6zu front=%-3s rear=%-3s len=%-3s "
           "nul=%-3s -> %s\n",
           cap, n, front_ok ? "ok" : "BAD", rear_ok ? "ok" : "BAD",
           len_ok ? "ok" : "BAD", nul_ok ? "ok" : "BAD", ok ? "PASS" : "FAIL");

    return ok ? 0 : 1;
}

int main(void)
{
    write_test_skills();
    g_buf = g_mem + PAD;

    static const size_t caps[] = { 1024, 1086, 64, 2, 0 };
    int failed = 0;

    printf("=== skill_loader_build_summary canary test (dir=%s) ===\n",
           AGENT_SKILLS_DIR);
    for (size_t i = 0; i < sizeof(caps) / sizeof(caps[0]); i++) {
        failed += run_case(caps[i]);
    }

    printf("=== %s ===\n", failed ? "FAILED" : "ALL PASS");
    return failed ? 1 : 0;
}
