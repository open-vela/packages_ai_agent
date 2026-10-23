/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "llm/llm_proxy.h"
#include "llm/llm_internal.h"
static int writes;
int claw_config_set(const char *key, const char *value)
{ assert(key && value); writes++; return 0; }
int main(void)
{
    char model[64], key[128], host[128];
    assert(llm_set_all("fixture.invalid", "/v1", "443", "dummy", "fixture") == 0);
    assert(writes == 5);
    assert(llm_set_all_runtime("runtime.invalid", "/v2", "8443", "dummy2", "runtime") == 0);
    assert(writes == 5);
    llm_snapshot_config(model, sizeof(model), key, sizeof(key), host, sizeof(host));
    assert(!strcmp(model, "runtime") && !strcmp(host, "runtime.invalid"));
    assert(!strcmp(key, "dummy2"));
    assert(!llm_set_all_runtime(NULL, "", NULL, NULL, NULL));
    assert(writes == 5);
    puts("test_llm_runtime_config: OK (runtime selection writes zero config fields, legacy persists)");
    return 0;
}
