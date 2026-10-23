/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../src/infra/vela_tls.c"
static unsigned calls;
static size_t written;
static int partial_write, source_error, source_zero, source_bad;
static volatile int cancel;
static int cancel_source;
static unsigned char pattern(size_t n) { return (unsigned char)(n * 17); }
int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *data, size_t n)
{
    (void)ssl;
    if (partial_write && n > 777) n = 777;
    for (size_t i = 0; i < n; i++) assert(data[i] == pattern(written+i));
    written += n; calls++; return (int)n;
}
static int source(void *arg, char *out, size_t n)
{
    size_t *pos = arg;
    if (source_error) return -1;
    if (source_zero) return 0;
    if (source_bad) return (int)n + 1;
    if (n > 71) n = 71;
    for (size_t i = 0; i < n; i++) out[i] = pattern((*pos)++);
    if (cancel_source) cancel = 1;
    return (int)n;
}
int main(void)
{
    tls_ctx_t ctx = {0}; ctx.net.fd = -1;
    size_t pos = 0;
    vela_body_src_t b = {.len=33001,.read=source,.ctx=&pos};
    vela_req_ctl_t ctl = {.cancel=&cancel};
    assert(!stream_send_body(&ctx,&b,&ctl));
    assert(written == b.len && pos == b.len && calls == 3);
    pos = written = calls = 0; partial_write = 1;
    assert(!stream_send_body(&ctx,&b,&ctl)); assert(written == b.len);
    partial_write = 0; pos = written = calls = 0;
    assert(!stream_send_body(&ctx,&b,NULL)); assert(calls == 9 && written == b.len);
    source_error=1; assert(stream_send_body(&ctx,&b,&ctl)==VELA_TLS_ERR_READ); source_error=0;
    source_zero=1; assert(stream_send_body(&ctx,&b,&ctl)==VELA_TLS_ERR_WRITE); source_zero=0;
    source_bad=1; assert(stream_send_body(&ctx,&b,&ctl)==VELA_TLS_ERR_OVERFLOW); source_bad=0;
    cancel=1; assert(stream_send_body(&ctx,&b,&ctl)==VELA_TLS_ERR_CANCELLED); cancel=0;
    cancel_source=1; assert(stream_send_body(&ctx,&b,&ctl)==VELA_TLS_ERR_CANCELLED); cancel_source=0; cancel=0;
    ctl.deadline_ms=agent_mono_ms()-1;
    assert(stream_send_body(&ctx,&b,&ctl)==VELA_TLS_ERR_TIMEOUT);
    puts("test_tls_stream_upload: OK (bounded coalescing, exact bytes, partial writes, legacy size, invalid/short sources, cancel/deadline)");
    return 0;
}
