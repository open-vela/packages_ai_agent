/* SPDX-License-Identifier: Apache-2.0 */
/* Exercise the real request builder with poisoned storage. Transport-only
 * tests cannot detect uninitialized headers introduced before the TLS call. */
#include <assert.h>
#include <stdio.h>
#include "../src/llm/llm_proxy.c"

int claw_config_set(const char *key, const char *value)
{
    (void)key;
    (void)value;
    assert(!"runtime configuration must not persist");
    return ERROR;
}

static void prepare(struct llm_stream_req *r, const char *model)
{
    assert(llm_set_all_runtime("fixture.invalid", "/v1", "443",
                               "test-credential", model) == OK);
    memset(r, 0xa5, sizeof(*r));
    assert(llm_stream_prepare(r, "{}", NULL) == OK);
    assert(!strcmp(r->auth_header, "Bearer test-credential"));
    assert(r->src == &r->mem_src && r->src->ctx == &r->mem);
    char buf[4];
    assert(r->src->read(r->src->ctx, buf, sizeof(buf)) == 2);
    assert(!memcmp(buf, "{}", 2));
    assert(r->src->read(r->src->ctx, buf, sizeof(buf)) == 0);
}

int main(void)
{
    struct llm_stream_req r;
    prepare(&r, "mimo-v2.6-flash");
    assert(r.provider[0] == '\0');
    assert(r.hdrs[2].name == NULL && r.hdrs[2].value == NULL);
    prepare(&r, "xiaomi/mimo-v2.6-flash");
    assert(!strcmp(r.provider, "xiaomi"));
    assert(!strcmp(r.hdrs[2].name, "X-Model-Provider-Id"));
    assert(!strcmp(r.hdrs[2].value, "xiaomi"));
    assert(r.hdrs[3].name == NULL);
    /* A following provider-less request must not retain the old route. */
    assert(llm_set_all_runtime(NULL, NULL, NULL, NULL,
                               "mimo-v2.5-tts") == OK);
    assert(llm_stream_prepare(&r, NULL, NULL) == OK);
    assert(r.provider[0] == '\0' && r.hdrs[2].name == NULL);
    assert(r.src == NULL);
    puts("test_llm_stream_prepare: OK (poisoned storage, provider termination, request reset)");
    return 0;
}
