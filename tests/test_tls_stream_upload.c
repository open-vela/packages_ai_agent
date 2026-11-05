/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../src/infra/vela_tls.c"
/* The timing contract under test is the real one, so the real reset
 * comes along instead of a local stub. */
#include "../src/infra/agent_timing.c"
static unsigned calls;
static size_t written;
static int partial_write, source_error, source_zero, source_bad;
static volatile int cancel;
static int cancel_source;
static unsigned char pattern(size_t n) { return (unsigned char)(n * 17); }

/* ── Full-path fixtures ───────────────────────────────────────────
 * The first half of this file drives stream_send_body alone.  The second
 * half runs vela_https_request_stream end to end against a scripted TLS
 * layer, so the P0 phase contract can be asserted: send covers headers
 * and body, ttfb is the first response byte, body runs from that byte,
 * the phases never overlap, and every exit path still reports
 * http_total. */

static int full_path;          /* accept arbitrary request bytes */
static int write_fail_now;     /* mbedtls_ssl_write fails outright */
static char response_script[2048];
static size_t script_len, script_pos;
static size_t script_slice = 64;
static int handshake_fail;

int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *data, size_t n)
{
    (void)ssl;
    if (write_fail_now) { calls++; return MBEDTLS_ERR_NET_SEND_FAILED; }
    if (partial_write && n > 777) n = 777;
    if (!full_path)
        for (size_t i = 0; i < n; i++) assert(data[i] == pattern(written+i));
    written += n; calls++; return (int)n;
}
int mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t n)
{
    (void)ssl;
    if (script_pos >= script_len) return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    size_t take = script_len - script_pos;
    if (take > n) take = n;
    if (take > script_slice) take = script_slice;
    memcpy(buf, response_script + script_pos, take);
    script_pos += take;
    return (int)take;
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

/* Connection-layer stubs: the handshake always succeeds (or fails on
 * demand) and the socket is a fake descriptor no syscall will accept,
 * which the code already tolerates because it ignores those results. */
void mbedtls_net_init(mbedtls_net_context *ctx) { ctx->fd = -1; }
int mbedtls_net_connect(mbedtls_net_context *ctx, const char *host, const char *port, int proto)
{ (void)host; (void)port; (void)proto; ctx->fd = 7; return 0; }
void mbedtls_net_close(mbedtls_net_context *ctx) { ctx->fd = -1; }
int mbedtls_net_set_block(mbedtls_net_context *ctx) { (void)ctx; return 0; }
int mbedtls_net_set_nonblock(mbedtls_net_context *ctx) { (void)ctx; return 0; }
void mbedtls_ctr_drbg_init(mbedtls_ctr_drbg_context *ctx) { (void)ctx; }
int mbedtls_ctr_drbg_seed(mbedtls_ctr_drbg_context *ctx,
    int (*f_entropy)(void *, unsigned char *, size_t), void *p_entropy,
    const unsigned char *custom, size_t len)
{ (void)ctx; (void)f_entropy; (void)p_entropy; (void)custom; (void)len; return 0; }
void mbedtls_ctr_drbg_free(mbedtls_ctr_drbg_context *ctx) { (void)ctx; }
int mbedtls_ctr_drbg_random(void *p_rng, unsigned char *output, size_t len)
{ (void)p_rng; (void)output; (void)len; return 0; }
int mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len)
{ (void)ctx; (void)buf; return (int)len; }
int mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len)
{ (void)ctx; (void)buf; (void)len; return 0; }
void mbedtls_ssl_init(mbedtls_ssl_context *ssl) { (void)ssl; }
void mbedtls_ssl_free(mbedtls_ssl_context *ssl) { (void)ssl; }
void mbedtls_ssl_config_init(mbedtls_ssl_config *conf) { (void)conf; }
void mbedtls_ssl_config_free(mbedtls_ssl_config *conf) { (void)conf; }
int mbedtls_ssl_config_defaults(mbedtls_ssl_config *conf, int endpoint, int transport, int preset)
{ (void)conf; (void)endpoint; (void)transport; (void)preset; return 0; }
void mbedtls_ssl_conf_authmode(mbedtls_ssl_config *conf, int authmode)
{ (void)conf; (void)authmode; }
void mbedtls_ssl_conf_rng(mbedtls_ssl_config *conf,
    int (*f_rng)(void *, unsigned char *, size_t), void *p_rng)
{ (void)conf; (void)f_rng; (void)p_rng; }
#ifdef MBEDTLS_SSL_ALPN
int mbedtls_ssl_conf_alpn_protocols(mbedtls_ssl_config *conf, const char **protos)
{ (void)conf; (void)protos; return 0; }
#endif
int mbedtls_ssl_setup(mbedtls_ssl_context *ssl, const mbedtls_ssl_config *conf)
{ (void)ssl; (void)conf; return 0; }
int mbedtls_ssl_set_hostname(mbedtls_ssl_context *ssl, const char *hostname)
{ (void)ssl; (void)hostname; return 0; }
void mbedtls_ssl_set_bio(mbedtls_ssl_context *ssl, void *ctx,
    int (*f_send)(void *, const unsigned char *, size_t),
    int (*f_recv)(void *, unsigned char *, size_t),
    int (*f_recv_timeout)(void *, unsigned char *, size_t, uint32_t))
{ (void)ssl; (void)ctx; (void)f_send; (void)f_recv; (void)f_recv_timeout; }
int mbedtls_ssl_handshake(mbedtls_ssl_context *ssl)
{ (void)ssl; return handshake_fail ? MBEDTLS_ERR_SSL_HANDSHAKE_FAILURE : 0; }
int mbedtls_ssl_close_notify(mbedtls_ssl_context *ssl) { (void)ssl; return 0; }
const char *mbedtls_ssl_get_version(const mbedtls_ssl_context *ssl)
{ (void)ssl; return "TLSv1.2"; }
const char *mbedtls_ssl_get_ciphersuite(const mbedtls_ssl_context *ssl)
{ (void)ssl; return "TLS-RSA-WITH-AES-128-GCM-SHA256"; }
#ifdef MBEDTLS_ERROR_C
void mbedtls_strerror(int errcode, char *buffer, size_t buflen)
{ (void)errcode; if (buflen > 0) buffer[0] = '\0'; }
#endif
bool http_proxy_is_enabled(void) { return false; }
int proxy_open_tunnel(const char *host, int port, int timeout_ms)
{ (void)host; (void)port; (void)timeout_ms; return -1; }

static void script_response(const char *text)
{
    script_len = strlen(text);
    assert(script_len < sizeof(response_script));
    memcpy(response_script, text, script_len);
    script_pos = 0;
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
    ctl.deadline_ms = 0;

    /* ── Full-path timing contract ────────────────────────────── */

    {
        static const char reply[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
        agent_llm_timing_t t;
        char resp[64];
        size_t out_len = 0;
        int status = 0;
        int rc;

        full_path = 1;
        written = calls = 0;
        script_response(reply);
        agent_timing_reset(&t);
        rc = vela_https_request_stream("fixture.invalid", "443", "POST",
            "/v1/chat", NULL, NULL, NULL, NULL, resp, sizeof(resp),
            &out_len, &status, &t);
        assert(rc == 0);
        assert(status == 200 && out_len == 5 && !strcmp(resp, "hello"));
        assert(t.streamed);
        assert(t.send_done_mono != 0);
        assert(t.send_ms != AGENT_T_NA && t.send_ms >= 0);
        assert(t.ttfb_ms != AGENT_T_NA && t.ttfb_ms >= 0);
        assert(t.body_ms != AGENT_T_NA && t.body_ms >= 0);
        assert(t.http_total_ms != AGENT_T_NA && t.http_total_ms >= 0);
        /* The phases partition the transport call: they may not overlap,
         * so their sum never exceeds the whole. */
        assert(t.send_ms + t.ttfb_ms + t.body_ms <= t.http_total_ms);
        full_path = 0;
    }

    /* The send fails outright: the paid attempt still lands in send_ms,
     * while ttfb and body honestly stay na. */
    {
        agent_llm_timing_t t;
        char resp[64];
        size_t out_len = 0;
        int status = 0;

        full_path = 1;
        write_fail_now = 1;
        written = calls = 0;
        script_response("");
        agent_timing_reset(&t);
        assert(vela_https_request_stream("fixture.invalid", "443", "POST",
            "/v1/chat", NULL, NULL, NULL, NULL, resp, sizeof(resp),
            &out_len, &status, &t) < 0);
        assert(t.send_ms != AGENT_T_NA && t.send_ms >= 0);
        assert(t.ttfb_ms == AGENT_T_NA);
        assert(t.body_ms == AGENT_T_NA);
        assert(t.http_total_ms != AGENT_T_NA && t.http_total_ms >= 0);
        write_fail_now = 0;
        full_path = 0;
    }

    /* Headers arrive, then the peer closes before the announced
     * Content-Length: ttfb was still paid and must survive, the
     * incomplete body stays na. */
    {
        static const char reply[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhe";
        agent_llm_timing_t t;
        char resp[64];
        size_t out_len = 0;
        int status = 0;

        full_path = 1;
        written = calls = 0;
        script_response(reply);
        agent_timing_reset(&t);
        assert(vela_https_request_stream("fixture.invalid", "443", "POST",
            "/v1/chat", NULL, NULL, NULL, NULL, resp, sizeof(resp),
            &out_len, &status, &t) == VELA_TLS_ERR_READ);
        assert(t.ttfb_ms != AGENT_T_NA && t.ttfb_ms >= 0);
        assert(t.body_ms == AGENT_T_NA);
        assert(t.http_total_ms != AGENT_T_NA && t.http_total_ms >= 0);
        full_path = 0;
    }

    puts("test_tls_stream_upload: OK (bounded coalescing, exact bytes, partial writes, legacy size, invalid/short sources, cancel/deadline, phase contract)");
    return 0;
}
