/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "tools/tool_files.h"

static void put(const char *p)
{ FILE *f = fopen(p, "w"); assert(f); fputs("original", f); fclose(f); }
static int write_path(const char *p)
{
    char json[512], out[256];
    snprintf(json, sizeof(json), "{\"path\":\"%s\",\"content\":\"changed\"}", p);
    return tool_write_file_execute(json, out, sizeof(out));
}
int main(void)
{
    char root[] = "/tmp/vg-file-policy-XXXXXX";
    assert(mkdtemp(root)); assert(!chdir(root));
    assert(!mkdir("volume", 0700)); assert(!symlink("volume", "data"));
    assert(!mkdir("volume/agent", 0700)); assert(!mkdir("volume/agent/config", 0700));
    assert(!mkdir("volume/agent/memory", 0700));
    assert(!mkdir("volume/velaguard", 0700));
    assert(!mkdir("volume/velaguard/reports", 0700));
    assert(!mkdir("volume/velaguard/config", 0700));
    put("volume/agent/config/config.json");
    put("volume/velaguard/config/points.json");
    put("volume/velaguard/reports/runtime-report.md");
    assert(write_path("data/agent/config/config.json") < 0);
    assert(write_path("data/velaguard/config/points.json") < 0);
    assert(write_path("data/velaguard/reports/runtime-report.md") < 0);
    assert(!symlink("../config", "volume/velaguard/reports/alias"));
    assert(write_path("data/velaguard/reports/alias/points.json") < 0);
    assert(!symlink("/tmp", "volume/agent/memory/escape"));
    assert(write_path("data/agent/memory/escape/nonexistent-vg-output") < 0);
    char out[256];
    assert(tool_read_file_execute("{\"path\":\"data/agent/config/config.json\"}", out, sizeof(out)) < 0);
    assert(write_path("data/velaguard/reports/daily-test.md") == 0);
    assert(write_path("data/agent/memory/test.md") == 0);
    for (size_t cap = 0; cap < 32; cap++) {
        unsigned char canary[40];
        memset(canary, 0xa5, sizeof(canary));
        tool_list_dir_execute("{}", (char *)canary + 1, cap);
        assert(canary[0] == 0xa5);
        for (size_t j = cap + 1; j < sizeof(canary); j++) assert(canary[j] == 0xa5);
    }
    unlink("volume/agent/config/config.json");
    unlink("volume/agent/memory/escape"); unlink("volume/agent/memory/test.md");
    unlink("volume/velaguard/reports/alias");
    unlink("volume/velaguard/config/points.json");
    unlink("volume/velaguard/reports/runtime-report.md");
    unlink("volume/velaguard/reports/daily-test.md");
    rmdir("volume/agent/config"); rmdir("volume/agent/memory"); rmdir("volume/agent");
    rmdir("volume/velaguard/config"); rmdir("volume/velaguard/reports");
    rmdir("volume/velaguard"); unlink("data"); rmdir("volume");
    assert(!chdir("/tmp")); assert(!rmdir(root));
    puts("test_file_policy: OK (symlink, missing target, config, firmware facts, allowed report)");
    return 0;
}
