/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include "../src/infra/vela_tls.c"
static int gets, offers, get_error, set_error;
static const char *version = "TLSv1.2";
void mbedtls_ssl_session_init(mbedtls_ssl_session *s) { memset(s, 0, sizeof(*s)); }
void mbedtls_ssl_session_free(mbedtls_ssl_session *s) { memset(s, 0, sizeof(*s)); }
int mbedtls_ssl_get_session(const mbedtls_ssl_context *s, mbedtls_ssl_session *d)
{ (void)s; (void)d; gets++; return get_error; }
int mbedtls_ssl_set_session(mbedtls_ssl_context *s, const mbedtls_ssl_session *d)
{ (void)s; (void)d; offers++; return set_error; }
const char *mbedtls_ssl_get_version(const mbedtls_ssl_context *s)
{ (void)s; return version; }
int main(void)
{
    mbedtls_ssl_context ssl = {0};
    vela_req_ctl_t ctl = {0};
    const char *host = "token-plan-cn.xiaomimimo.com";
    tls_voice_session_clear();
    assert(!tls_voice_session_offer(&ssl, host, "443", &ctl, 100));
    tls_voice_session_save(&ssl, "other.example", "443", &ctl, 100);
    tls_voice_session_save(&ssl, host, "8443", &ctl, 100);
    tls_voice_session_save(&ssl, host, "443", NULL, 100);
    assert(gets == 0);
    tls_voice_session_save(&ssl, host, "443", &ctl, 100);
    assert(gets == 1 && voice_session_valid);
    assert(!tls_voice_session_offer(&ssl, "other.example", "443", &ctl, 101));
    assert(!tls_voice_session_offer(&ssl, host, "8443", &ctl, 101));
    assert(!tls_voice_session_offer(&ssl, host, "443", NULL, 101));
    assert(offers == 0);
    assert(tls_voice_session_offer(&ssl, host, "443", &ctl, 60099));
    assert(!tls_voice_session_offer(&ssl, host, "443", &ctl, 60100));
    assert(!voice_session_valid && offers == 1);
    tls_voice_session_save(&ssl, host, "443", &ctl, UINT32_MAX - 100);
    assert(tls_voice_session_offer(&ssl, host, "443", &ctl, 100));
    set_error = -1;
    assert(!tls_voice_session_offer(&ssl, host, "443", &ctl, 101));
    tls_voice_session_clear();
    assert(!voice_session_valid);
    get_error = -1;
    tls_voice_session_save(&ssl, host, "443", &ctl, 200);
    assert(!voice_session_valid);
    get_error = 0; version = "TLSv1.3";
    int before = gets;
    tls_voice_session_save(&ssl, host, "443", &ctl, 300);
    assert(gets == before && !voice_session_valid);
    puts("test_tls_voice_session: OK (host/port/control isolation, expiry, clock wrap, API failures, clear, TLS 1.3 excluded)");
    return 0;
}
