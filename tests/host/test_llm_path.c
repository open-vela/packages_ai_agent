/* Host check for llm_complete_path():
 *   cc -Isrc tests/host/test_llm_path.c -o /tmp/test_llm_path && /tmp/test_llm_path */

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "channels/llm_path.h"

static void check(const char* in, const char* want)
{
    char buf[256];
    const char* got = llm_complete_path(in, buf, sizeof(buf));
    if (strcmp(got, want) != 0) {
        fprintf(stderr, "FAIL %s -> %s (want %s)\n", in, got, want);
        assert(0);
    }
}

int main(void)
{
    /* Previous behaviour kept */
    check("/v1", "/v1/chat/completions");
    check("/v1/", "/v1/chat/completions");
    check("/v1/chat/completions", "/v1/chat/completions");
    check("/v1/messages", "/v1/messages");

    /* Version segments behind a prefix */
    check("/compatible-mode/v1", "/compatible-mode/v1/chat/completions");
    check("/compatible-mode/v1/", "/compatible-mode/v1/chat/completions");
    check("/api/v3", "/api/v3/chat/completions");
    check("/api/paas/v4", "/api/paas/v4/chat/completions");

    /* Not a version segment: unchanged */
    check("/api/v3/responses", "/api/v3/responses");
    check("/v1beta", "/v1beta");
    check("/v", "/v");
    check("/", "/");
    check("/custom/proxy", "/custom/proxy");

    puts("llm_complete_path: all checks passed");
    return 0;
}
