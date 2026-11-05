/*
 * Host regression test for the RSP-01 timing contract.
 *
 * The board trace used to print one number per ReAct step and call it a tool
 * latency, so a 97 s model response read as a 97 s tool.  The fixes that must
 * stay fixed here:
 *
 *   1. A phase that did not run is reported as "na", never as zero, so an
 *      unmeasured phase cannot be averaged in as an instant one.
 *   2. DNS and TCP are labelled "(fused)" because mbedtls_net_connect()
 *      does both in one call.  The label may not silently disappear.
 *   3. The line never contains the request body, headers or a key.  Only
 *      sizes, status codes and timings are allowed.
 *   4. A line that would not fit its buffer is refused, not truncated: a
 *      truncated line still parses and would read as a complete record.
 *   5. Monotonic elapsed math survives the 32-bit wrap.
 *
 * 用法：
 *   gcc -O2 -g -Wall -Wextra -include tests/stub/host_compat.h \
 *       -I tests/stub -I include -I src \
 *       tests/test_agent_timing.c src/infra/agent_timing.c \
 *       -o /tmp/test_agent_timing
 *   /tmp/test_agent_timing
 *
 * 退出码非 0 表示至少一个用例失败。
 */

#include "infra/agent_timing.h"

#include <stdio.h>
#include <string.h>

static int s_failures;

static void fail(const char* what)
{
    printf("FAIL: %s\n", what);
    s_failures++;
}

static void expect_contains(const char* line, const char* needle)
{
    if (strstr(line, needle) == NULL) {
        printf("FAIL: expected '%s' in: %s\n", needle, line);
        s_failures++;
    }
}

static void expect_absent(const char* line, const char* needle)
{
    if (strstr(line, needle) != NULL) {
        printf("FAIL: '%s' must not appear in: %s\n", needle, line);
        s_failures++;
    }
}

/* A phase that did not run must not look like a fast one. */

static void test_unmeasured_is_na(void)
{
    agent_llm_timing_t t;
    char line[512];
    int n;

    agent_timing_reset(&t);
    n = agent_timing_format_http(line, sizeof(line), 0, &t);
    if (n <= 0) {
        fail("format returned <= 0 for a reset struct");
        return;
    }

    expect_contains(line, "dns_tcp=na(fused)");
    expect_contains(line, "tls=na");
    expect_contains(line, "send=na");
    expect_contains(line, "ttfb=na");
    expect_contains(line, "body=na");
    expect_contains(line, "pool=na");
    expect_contains(line, "build=na");
    expect_contains(line, "parse=na");
    expect_contains(line, "http=na");
    expect_contains(line, "total=na");

    /* backoff is not a phase: zero really means no sleep happened. */

    expect_contains(line, "backoff=0");

    /* Nothing invented a measurement. */

    expect_absent(line, "dns_tcp=0");
    expect_absent(line, "ttfb=0");
}

/* Real numbers replace na, and the fused label stays. */

static void test_measured_values(void)
{
    agent_llm_timing_t t;
    char line[512];

    agent_timing_reset(&t);
    t.pool_ms = 0;
    t.dns_tcp_ms = 42;
    t.tls_setup_ms = 3;
    t.tls_ms = 1180;
    t.send_ms = 7;
    t.ttfb_ms = 9100;
    t.body_ms = 240;
    t.build_ms = 12;
    t.parse_ms = 5;
    t.http_total_ms = 10584;
    t.llm_total_ms = 10601;
    t.req_bytes = 16384;
    t.http_rc = 200;
    t.http_status = 200;
    t.parse_ok = true;
    t.reused = true;
    t.attempt = 2;
    t.backoff_ms = 2000;

    if (agent_timing_format_http(line, sizeof(line), 3, &t) <= 0) {
        fail("format rejected a fully populated struct");
        return;
    }

    expect_contains(line, "iter=3");
    expect_contains(line, "attempt=2");
    expect_contains(line, "reused=1");
    expect_contains(line, "proxy=0");
    expect_contains(line, "dns_tcp=42(fused)");
    expect_contains(line, "tls=1180");
    expect_contains(line, "ttfb=9100");
    expect_contains(line, "body=240");
    expect_contains(line, "pool=0");
    expect_contains(line, "parse=5");
    expect_contains(line, "req=16384");
    expect_contains(line, "status=200");
    expect_contains(line, "parse_ok=1");
    expect_contains(line, "backoff=2000");
}

/* The proxy path cannot see inside its tunnel; it must say na, not zero. */

static void test_proxy_phases_are_na(void)
{
    agent_llm_timing_t t;
    char line[512];

    agent_timing_reset(&t);
    t.proxy = true;
    t.http_total_ms = 8000;
    agent_timing_no_transport(&t);

    if (agent_timing_format_http(line, sizeof(line), 0, &t) <= 0) {
        fail("format rejected the proxy struct");
        return;
    }

    expect_contains(line, "proxy=1");
    expect_contains(line, "dns_tcp=na(fused)");
    expect_contains(line, "tls=na");
    expect_contains(line, "http=8000");
}

/* A line that does not fit is refused, so a short read is never mistaken for
 * a complete record. */

static void test_short_buffer_is_refused(void)
{
    agent_llm_timing_t t;
    char small[64];

    agent_timing_reset(&t);
    if (agent_timing_format_http(small, sizeof(small), 0, &t) != -1) {
        fail("format accepted a buffer too small for the line");
    }
}

/* The line must describe the request, not its contents. */

static void test_no_payload_leaks(void)
{
    agent_llm_timing_t t;
    char line[512];

    agent_timing_reset(&t);
    t.req_bytes = 999;
    t.http_status = 429;

    if (agent_timing_format_http(line, sizeof(line), 1, &t) <= 0)
        return;

    expect_absent(line, "Bearer");
    expect_absent(line, "Authorization");
    expect_absent(line, "api_key");
    expect_absent(line, "content");
}

/* agent_elapsed_ms must stay correct across the 32-bit millisecond wrap. */

static void test_elapsed_wrap(void)
{
    uint32_t before = 0xffffffe0u; /* 32 ms before the wrap */
    int32_t ms = agent_elapsed_ms(before, 0x00000010u);

    if (ms != 48) {
        printf("FAIL: wrap elapsed was %ld, expected 48\n", (long)ms);
        s_failures++;
    }

    /* More than 24 days apart can only be a mixed clock base. */

    if (agent_elapsed_ms(0, 0x80000000u) != AGENT_T_NA) {
        fail("implausibly large delta was not reported as na");
    }
}

/* agent_lap_ms advances its mark, so consecutive phases do not overlap. */

static void test_lap_advances(void)
{
    uint32_t mark = agent_mono_ms();
    int32_t first = agent_lap_ms(&mark);
    uint32_t seen = mark;
    int32_t second = agent_lap_ms(&mark);

    if (first < 0 || second < 0) {
        fail("lap returned a negative elapsed time");
    }

    if (mark < seen) {
        fail("lap moved the mark backwards");
    }
}

/* Streaming observation: first_data is na until a sink saw an SSE data
 * line, stream=0 until the request went through the streaming entry, and
 * send_done 0 means "unknown" rather than a real instant.  no_transport
 * clears the observation along with the other transport phases. */

static void test_stream_fields(void)
{
    agent_llm_timing_t t;
    char line[512];

    agent_timing_reset(&t);
    if (t.first_data_ms != AGENT_T_NA) {
        fail("reset left first_data_ms measured");
    }
    if (t.send_done_mono != 0) {
        fail("reset left send_done_mono set");
    }
    if (t.streamed) {
        fail("reset left streamed set");
    }

    if (agent_timing_format_http(line, sizeof(line), 0, &t) <= 0) {
        fail("format returned <= 0 for a reset struct");
        return;
    }

    expect_contains(line, "first_data=na");
    expect_contains(line, "stream=0");

    agent_timing_reset(&t);
    t.first_data_ms = 320;
    t.send_done_mono = 123456u;
    t.streamed = true;
    if (agent_timing_format_http(line, sizeof(line), 1, &t) <= 0) {
        fail("format rejected a streaming struct");
        return;
    }

    expect_contains(line, "first_data=320");
    expect_contains(line, "stream=1");

    agent_timing_no_transport(&t);
    if (t.first_data_ms != AGENT_T_NA || t.streamed
        || t.send_done_mono != 0) {
        fail("no_transport left the streaming observation behind");
    }
}

int main(void)
{
    test_unmeasured_is_na();
    test_measured_values();
    test_proxy_phases_are_na();
    test_short_buffer_is_refused();
    test_no_payload_leaks();
    test_elapsed_wrap();
    test_lap_advances();
    test_stream_fields();

    if (s_failures != 0) {
        printf("%d failure(s)\n", s_failures);
        return 1;
    }

    printf("test_agent_timing: all cases passed\n");
    return 0;
}
