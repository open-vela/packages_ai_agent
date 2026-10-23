/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
/* Exercise the actual BIO callback, including handshake use. */
#include "../src/infra/vela_tls.c"
static int injected_error;
int mbedtls_net_send(void *arg, const unsigned char *buf, size_t len)
{
    (void)arg; (void)buf;
    errno = injected_error;
    return injected_error ? MBEDTLS_ERR_NET_SEND_FAILED : (int)len;
}
int main(void)
{
    tls_ctx_t ctx = {0};
    unsigned char data[3] = {0};
    injected_error = ENOMEM;
    assert(tls_controlled_send(&ctx, data, sizeof(data)) == MBEDTLS_ERR_SSL_WANT_WRITE);
    assert(ctx.send_pressure == 1);
    injected_error = 0;
    assert(tls_controlled_send(&ctx, data, sizeof(data)) == 3);
    int fatal[] = {ENOTCONN, EPIPE, ECONNRESET, EBADF, ENOTSOCK};
    for (unsigned i = 0; i < sizeof(fatal)/sizeof(fatal[0]); i++) {
        injected_error = fatal[i];
        assert(tls_controlled_send(&ctx, data, sizeof(data)) == MBEDTLS_ERR_NET_SEND_FAILED);
    }
    puts("test_tls_send_pressure: OK (ENOMEM -> WANT_WRITE at BIO, successful retry, fatal errors unchanged)");
    return 0;
}
