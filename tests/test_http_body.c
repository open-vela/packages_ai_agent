/*
 * Host regression test for HTTP/1.1 body end detection.
 *
 * The TLS reader used to end a body only on Content-Length.  A chunked reply
 * carries no Content-Length, so on a keep-alive connection the reader kept
 * reading after the terminating zero chunk and sat there until the socket
 * timed out.  On the board that shows up as a body_ms in the tens of seconds
 * that nothing in the response explains.
 *
 * The cases below are the ones that decide whether that wait exists:
 *
 *   1. Content-Length, whole body already buffered
 *   2. Content-Length, body arriving in pieces
 *   3. chunked, whole body already buffered
 *   4. chunked, body arriving in pieces
 *   5. connection closed with no framing
 *   6. keep-alive with no framing (cannot end without the peer closing)
 *   7. truncated body: short of Content-Length
 *   8. truncated chunked body: never reaches the zero chunk
 *   9. a "0" byte inside chunk data is not a terminator
 *  10. chunk size spanning a partially buffered size line
 *
 * 用法：
 *   gcc -O2 -g -Wall -Wextra -I src tests/test_http_body.c -o /tmp/test_http_body
 *   /tmp/test_http_body
 *
 * 退出码非 0 表示至少一个用例失败。
 */

#include "infra/http_body.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

static void expect(bool got, bool want, const char* what)
{
    if (got != want) {
        printf("FAIL: %s (got %d, want %d)\n", what, got, want);
        s_failures++;
    }
}

/* 1. Content-Length with the whole body already in the buffer.  No read
 *    should be needed at all. */

static void test_content_length_complete(void)
{
    const char* body = "{\"ok\":true}";

    expect(vela_http_body_complete(false, 11, strlen(body), body), true,
        "content-length: complete body is complete");
}

/* 2. Content-Length with the body arriving in pieces. */

static void test_content_length_partial(void)
{
    const char* body = "{\"ok\":tru";

    expect(vela_http_body_complete(false, 11, strlen(body), body), false,
        "content-length: partial body is not complete");

    expect(vela_http_body_complete(false, 11, 11, "{\"ok\":true}"), true,
        "content-length: exactly the declared length is complete");

    /* More bytes than declared cannot happen on a framed read, but the
     * predicate must not report incomplete for it. */

    expect(vela_http_body_complete(false, 11, 20, "{\"ok\":true}......"), true,
        "content-length: overshoot counts as complete");
}

/* 3. chunked, whole body already buffered.  This is the case that used to
 *    hang: framing was complete but nothing looked at it. */

static void test_chunked_complete(void)
{
    /* "hello" in one chunk, then the terminator. */

    const char* body = "5\r\nhello\r\n0\r\n\r\n";

    expect(vela_http_body_complete(true, -1, strlen(body), body), true,
        "chunked: terminator present means complete");

    /* Several chunks. */

    const char* multi = "3\r\nabc\r\n3\r\ndef\r\n0\r\n\r\n";

    expect(vela_http_body_complete(true, -1, strlen(multi), multi), true,
        "chunked: multiple chunks then terminator is complete");
}

/* 4. chunked, body arriving in pieces. */

static void test_chunked_partial(void)
{
    const char* no_terminator = "5\r\nhello\r\n";

    expect(vela_http_body_complete(true, -1, strlen(no_terminator),
               no_terminator),
        false, "chunked: missing terminator is not complete");

    const char* size_only = "5\r\n";

    expect(vela_http_body_complete(true, -1, strlen(size_only), size_only),
        false, "chunked: chunk data still outstanding is not complete");

    const char* data_no_trailer = "5\r\nhello";

    expect(vela_http_body_complete(true, -1, strlen(data_no_trailer),
               data_no_trailer),
        false, "chunked: missing trailing CRLF is not complete");
}

/* 5 and 6. No framing at all.  The predicate cannot claim completion: only
 * the peer closing ends such a body. */

static void test_unframed(void)
{
    const char* body = "{\"partial\":1";

    expect(vela_http_body_complete(false, -1, strlen(body), body), false,
        "unframed: never complete on its own");

    expect(vela_http_body_unframed(false, -1), true,
        "unframed: reported as unframed");
    expect(vela_http_body_unframed(true, -1), false,
        "chunked is framed");
    expect(vela_http_body_unframed(false, 0), false,
        "content-length zero is framed");
}

/* 7. Content-Length announced but the body is short: the reader must keep
 *    reading, not accept a short body as complete. */

static void test_truncated_content_length(void)
{
    const char* body = "{\"ok\":";

    expect(vela_http_body_complete(false, 100, strlen(body), body), false,
        "truncated: short of Content-Length is not complete");
}

/* 8. chunked with a truncated final chunk: never reaches the zero chunk. */

static void test_truncated_chunked(void)
{
    const char* body = "5\r\nhel";

    expect(vela_http_body_complete(true, -1, strlen(body), body), false,
        "truncated chunked: incomplete chunk is not complete");
}

/* 9. A zero byte inside chunk data must not be read as the terminator.  The
 *    parser walks chunk sizes in order, so the literal "0" in the payload is
 *    just data. */

static void test_zero_inside_payload(void)
{
    /* Chunk of 3 bytes containing "a0b", then a normal terminator. */

    const char* body = "3\r\na0b\r\n0\r\n\r\n";

    expect(vela_http_body_complete(true, -1, strlen(body), body), true,
        "zero inside payload: still ends at the real terminator");

    /* Same payload but without the real terminator: must NOT be complete,
     * even though the body contains a "0" byte. */

    const char* no_term = "3\r\na0b\r\n";

    expect(vela_http_body_complete(true, -1, strlen(no_term), no_term), false,
        "zero inside payload: not mistaken for the terminator");
}

/* 10. A chunk size line split across reads must not be parsed as complete. */

static void test_partial_size_line(void)
{
    /* "10\r" with no LF yet. */

    const char* body = "10\r";

    expect(vela_http_body_complete(true, -1, strlen(body), body), false,
        "partial size line is not complete");
}

/* Zero-length response bodies are complete immediately. */

static void test_empty_bodies(void)
{
    expect(vela_http_body_complete(false, 0, 0, ""), true,
        "content-length 0 with no bytes is complete");

    expect(vela_http_body_complete(true, -1, 0, ""), false,
        "chunked with no bytes yet is not complete");
}

int main(void)
{
    test_content_length_complete();
    test_content_length_partial();
    test_chunked_complete();
    test_chunked_partial();
    test_unframed();
    test_truncated_content_length();
    test_truncated_chunked();
    test_zero_inside_payload();
    test_partial_size_line();
    test_empty_bodies();

    if (s_failures != 0) {
        printf("%d failure(s)\n", s_failures);
        return 1;
    }

    printf("test_http_body: all cases passed\n");
    return 0;
}
