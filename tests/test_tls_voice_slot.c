/* SPDX-License-Identifier: Apache-2.0 */
/* P2 phase 1: the voice-dedicated keep-alive slot, driven end to end
 * through the same scripted TLS layer test_tls_stream_upload uses.  The
 * scripts give every response an explicit Content-Length (or chunked
 * framing), and an exhausted script reads as WANT_READ — a quiet, alive
 * connection — so the acquire-time probe and the post-sink drain behave
 * exactly like a real server that has nothing more to send.
 *
 * Covered: the keep-decision truth table, sequential reuse on one slot
 * (timing.reused, handshake phases NA), the keep-alive vs close request
 * headers, sink-early + bounded drain publishing, the one transparent
 * retry after a reused connection fails before any response byte, and
 * preconnect warming the slot. */

#define CONFIG_AI_AGENT_VOICE_CONN_SLOT 1

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include "../src/infra/vela_tls.c"
#include "../src/infra/agent_timing.c"

static char response_script[2048];
static size_t script_len, script_pos;
static int fail_next_read;
static int net_connect_calls;
static char reqlog[4096];
static size_t reqlog_n;

/* The script models the server's future output, not the socket buffer:
 * bytes only become readable after the request has been written, so an
 * acquire-time liveness probe on a quiet connection reads WANT_READ
 * instead of eating the next response's status line. */
static int request_seen;

int mbedtls_ssl_write(mbedtls_ssl_context *ssl, const unsigned char *data, size_t n)
{
    (void)ssl;
    if (reqlog_n + n < sizeof(reqlog))
    {
        memcpy(reqlog + reqlog_n, data, n);
        reqlog_n += n;
        reqlog[reqlog_n] = '\0';
    }

    request_seen = 1;
    return (int)n;
}

int mbedtls_ssl_read(mbedtls_ssl_context *ssl, unsigned char *buf, size_t n)
{
    size_t take;

    (void)ssl;
    if (!request_seen || script_pos >= script_len)
    {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }

    if (fail_next_read)
    {
        /* A hard read failure the transport cannot retry: errno must not
         * look like EAGAIN, which would make NET_RECV_FAILED retryable. */
        fail_next_read = 0;
        errno = EBADF;
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }

    take = script_len - script_pos;
    if (take > n) take = n;
    memcpy(buf, response_script + script_pos, take);
    script_pos += take;
    return (int)take;
}

void mbedtls_net_init(mbedtls_net_context *ctx) { ctx->fd = -1; }
int mbedtls_net_connect(mbedtls_net_context *ctx, const char *host, const char *port, int proto)
{ (void)host; (void)port; (void)proto; net_connect_calls++; ctx->fd = 7; return 0; }
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
{ (void)ssl; return 0; }
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
    request_seen = 0;
}

static void reqlog_reset(void)
{
    reqlog_n = 0;
    reqlog[0] = '\0';
}

/* A sink that reports the response verified-complete on its first data:
 * the TTS behaviour.  The chunked terminator stays unread. */
static char sink_got[64];
static size_t sink_got_n;
static int early_sink(void *ctx, const char *data, size_t len)
{
    (void)ctx;
    assert(len < sizeof(sink_got));
    memcpy(sink_got, data, len);
    sink_got_n = len;
    return 1;
}

int main(void)
{
    agent_llm_timing_t t;
    char resp[64];
    size_t out_len;
    int status;
    int rc;

    /* ── keep-decision truth table ─────────────────────────────── */

    assert(voice_slot_keep(0, true, false, false));
    assert(voice_slot_keep(0, true, true, false));
    assert(voice_slot_keep(0, false, true, true));
    assert(!voice_slot_keep(0, false, true, false));
    assert(!voice_slot_keep(0, false, false, true));
    assert(!voice_slot_keep(VELA_TLS_ERR_READ, true, true, true));

    /* ── first request: fresh connect, keep-alive header, published ── */

    script_response("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello");
    reqlog_reset();
    agent_timing_reset(&t);
    rc = vela_https_request_stream_voice("fixture.invalid", "443", "POST",
        "/v1/x", NULL, NULL, NULL, NULL, resp, sizeof(resp), &out_len,
        &status, &t);
    assert(rc == 0);
    assert(status == 200 && out_len == 5 && strcmp(resp, "hello") == 0);
    assert(!t.reused);
    assert(net_connect_calls == 1);
    assert(strstr(reqlog, "Connection: keep-alive") != NULL);

    /* ── second request: reuses the slot, no handshake, still ok ── */

    script_response("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nworld");
    reqlog_reset();
    agent_timing_reset(&t);
    rc = vela_https_request_stream_voice("fixture.invalid", "443", "POST",
        "/v1/x", NULL, NULL, NULL, NULL, resp, sizeof(resp), &out_len,
        &status, &t);
    assert(rc == 0);
    assert(strcmp(resp, "world") == 0);
    assert(t.reused);
    assert(t.dns_tcp_ms == AGENT_T_NA && t.tls_ms == AGENT_T_NA);
    assert(net_connect_calls == 1);
    assert(strstr(reqlog, "Connection: keep-alive") != NULL);

    /* ── sink-early + bounded drain still publishes ────────────── */

    script_response("HTTP/1.1 200 OK\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n"
                    "5\r\nhello\r\n0\r\n\r\n");
    reqlog_reset();
    agent_timing_reset(&t);
    sink_got_n = 0;
    {
        vela_resp_sink_t s = { .write = early_sink, .ctx = NULL };

        rc = vela_https_request_stream_voice("fixture.invalid", "443", "POST",
            "/v1/x", NULL, NULL, &s, NULL, resp, sizeof(resp), &out_len,
            &status, &t);
    }
    assert(rc == 0);
    assert(sink_got_n == 5 && strncmp(sink_got, "hello", 5) == 0);

    /* the slot survived the drain: the next request reuses it */

    script_response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    agent_timing_reset(&t);
    rc = vela_https_request_stream_voice("fixture.invalid", "443", "POST",
        "/v1/x", NULL, NULL, NULL, NULL, resp, sizeof(resp), &out_len,
        &status, &t);
    assert(rc == 0 && t.reused && net_connect_calls == 1);

    /* ── a reused connection failing before any response byte is
     * retried once on a fresh connection ───────────────────────── */

    fail_next_read = 1;
    script_response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    reqlog_reset();
    agent_timing_reset(&t);
    rc = vela_https_request_stream_voice("fixture.invalid", "443", "POST",
        "/v1/x", NULL, NULL, NULL, NULL, resp, sizeof(resp), &out_len,
        &status, &t);
    assert(rc == 0 && strcmp(resp, "ok") == 0);
    assert(net_connect_calls == 2);
    /* the failed attempt asked keep-alive; the transparent retry rides
     * the plain entry and closes */
    assert(strstr(reqlog, "Connection: keep-alive") != NULL);
    assert(strstr(reqlog, "Connection: close") != NULL);

    /* ── preconnect warms the slot without a request ───────────── */

    vela_tls_pool_cleanup();
    net_connect_calls = 0;
    assert(vela_https_voice_preconnect("fixture.invalid", "443", NULL, NULL) == 0);
    assert(net_connect_calls == 1);

    script_response("HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nhot");
    agent_timing_reset(&t);
    rc = vela_https_request_stream_voice("fixture.invalid", "443", "POST",
        "/v1/x", NULL, NULL, NULL, NULL, resp, sizeof(resp), &out_len,
        &status, &t);
    assert(rc == 0 && t.reused && net_connect_calls == 1);

    /* ── the plain entry still closes (refactor regression) ────── */

    script_response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\ncl");
    reqlog_reset();
    rc = vela_https_request_stream("fixture.invalid", "443", "POST",
        "/v1/x", NULL, NULL, NULL, NULL, resp, sizeof(resp), &out_len,
        &status, &t);
    assert(rc == 0);
    assert(strstr(reqlog, "Connection: close") != NULL);
    assert(strstr(reqlog, "Connection: keep-alive") == NULL);

    /* the plain entry pays its own connection and leaves the voice
     * slot untouched: the next voice request still reuses */

    assert(net_connect_calls == 2);   /* preconnect + plain */
    script_response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    agent_timing_reset(&t);
    rc = vela_https_request_stream_voice("fixture.invalid", "443", "POST",
        "/v1/x", NULL, NULL, NULL, NULL, resp, sizeof(resp), &out_len,
        &status, &t);
    assert(rc == 0 && t.reused);
    assert(net_connect_calls == 2);

    vela_tls_pool_cleanup();

    puts("test_tls_voice_slot: OK (keep truth table, sequential reuse, "
         "keep-alive/close headers, sink-early drain, one retry, "
         "preconnect, plain-entry isolation)");
    return 0;
}
