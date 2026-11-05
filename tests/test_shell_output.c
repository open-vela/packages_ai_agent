/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include "../src/tools/tool_shell.c"

int main(void)
{
    unsigned char raw[4100];
    size_t sizes[] = {0, 1, 17, 64, 4096};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        size_t cap = sizes[i];
        memset(raw, 0xa5, sizeof(raw));
        builtin_df((char *)raw + 1, cap);
        assert(raw[0] == 0xa5);
        for (size_t j = cap + 1; j < sizeof(raw); j++) assert(raw[j] == 0xa5);
        memset(raw, 0xa5, sizeof(raw));
        builtin_ps((char *)raw + 1, cap);
        assert(raw[0] == 0xa5);
        for (size_t j = cap + 1; j < sizeof(raw); j++) assert(raw[j] == 0xa5);
        memset(raw, 0xa5, sizeof(raw));
        tool_run_shell_execute("{\"command\":\"df\"}", (char *)raw + 1, cap);
        assert(raw[0] == 0xa5);
        for (size_t j = cap + 1; j < sizeof(raw); j++) assert(raw[j] == 0xa5);
    }
    const char *blocked[] = {
        "vgmodbus -a 1 -r 0 -n 0", "vgpoint get", "vgcfg commit changed",
        "cat /data/agent/config/config.json", "head /data/agent/config/config.json"
    };
    for (size_t i = 0; i < sizeof(blocked) / sizeof(blocked[0]); i++) {
        char json[256], out[512];
        snprintf(json, sizeof(json), "{\"command\":\"%s\"}", blocked[i]);
        assert(tool_run_shell_execute(json, out, sizeof(out)) == ERROR);
    }
    assert(is_blocked("date -s 'Oct 18 20:00:00 2026'"));
    assert(is_blocked("date -sOct 18 20:00:00 2026"));
    assert(is_blocked("date -u -sOct 18 20:00:00 2026"));
    assert(is_blocked("date +%s -s 'Oct 18 20:00:00 2026'"));
    assert(is_blocked("date -u +%s -s 'Oct 18 20:00:00 2026'"));
    assert(is_blocked("date +%s\t-s 'Oct 18 20:00:00 2026'"));
    assert(is_blocked("date +%s extra"));
    assert(is_blocked("date -u +%s extra"));
    assert(is_blocked("date -u -u"));
    assert(is_blocked("date +"));
    assert(!is_blocked("date"));
    assert(!is_blocked("date   "));
    assert(!is_blocked("date -u"));
    assert(!is_blocked("date -u   "));
    assert(!is_blocked("date +%s"));
    assert(!is_blocked("date +%s   "));
    assert(!is_blocked("date -u +%s"));
    puts("test_shell_output: OK (exact canaries, metadata bounds, product allowlist)");
    return 0;
}
