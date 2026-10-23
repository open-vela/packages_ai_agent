/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "llm/llm_proxy.h"
#include "llm/llm_internal.h"
#include "infra/vela_tls.h"
static int writes;
int claw_config_set(const char *key, const char *value)
{ assert(key && value); writes++; return 0; }
static int probes;
int vela_https_request_stream(const char *host,const char *port,const char *method,
 const char *path,const vela_header_t *headers,const vela_body_src_t *body,
 const vela_resp_sink_t *sink,const vela_req_ctl_t *ctl,char *resp,size_t cap,
 size_t *bytes,int *status,agent_llm_timing_t *timing)
{
    (void)sink;(void)ctl;(void)resp;(void)cap;(void)bytes;(void)timing;
    assert(!strcmp(host,"fixture.invalid") && !strcmp(port,"8443"));
    assert(!strcmp(method,"POST") && !strcmp(path,"/asr429"));
    bool auth=false;
    for (int i=0;headers[i].name;i++) if (!strcmp(headers[i].name,"Authorization")) {
        assert(!strcmp(headers[i].value,"Bearer velaguard-invalid-test-credential"));auth=true;
    }
    assert(auth && body && body->len==4);
    char b[4]; assert(body->read(body->ctx,b,sizeof(b))==4 && !memcmp(b,"test",4));
    probes++; *status=429; return 0;
}
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
    int status=0; size_t bytes=0; char response[8];
    assert(!agent_http_probe_post_stream("fixture.invalid","8443","/asr429","test",NULL,NULL,response,sizeof(response),&bytes,&status,NULL));
    assert(probes==1 && status==429 && writes==5);
    assert(agent_http_probe_post_stream("bad\r\nhost","8443","/asr429","test",NULL,NULL,response,sizeof(response),&bytes,&status,NULL)<0);
    assert(probes==1);
    puts("test_llm_runtime_config: OK (runtime selection writes zero config fields, legacy persists)");
    return 0;
}
