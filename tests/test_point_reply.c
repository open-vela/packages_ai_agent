/*
 * Host regression test for the deterministic single-point reply.
 *
 * The shortcut exists to remove one model round-trip per point query.  What
 * must stay true:
 *
 *   1. A successful sample is echoed exactly: name, value, unit, age.
 *   2. Stale data says so; it must never read as a current value.
 *   3. Any shape that needs wording returns NULL so the LLM still handles
 *      it - ambiguity, no table, no sample, a failed read, partial lines.
 *   4. The value is not rounded, rescaled or reformatted.
 *
 * 用法：
 *   gcc -O2 -g -Wall -Wextra -I src \
 *       tests/test_point_reply.c src/core/point_reply.c -o /tmp/test_point_reply
 *   /tmp/test_point_reply
 *
 * 退出码非 0 表示至少一个用例失败。
 */

#include "core/point_reply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int s_failures;

static void expect_null(const char* input, const char* what)
{
    char* out = vg_point_reply_from_tool_output(input);

    if (out != NULL) {
        printf("FAIL: %s should fall through, got: %s\n", what, out);
        s_failures++;
        free(out);
    }
}

static void expect_has(const char* input, const char* what,
    const char* needles[], int n)
{
    char* out = vg_point_reply_from_tool_output(input);
    int i;

    if (out == NULL) {
        printf("FAIL: %s returned NULL\n", what);
        s_failures++;
        return;
    }

    for (i = 0; i < n; i++) {
        if (strstr(out, needles[i]) == NULL) {
            printf("FAIL: %s missing '%s' in: %s\n", what, needles[i], out);
            s_failures++;
        }
    }

    free(out);
}

/* 1. A successful sample keeps every field the operator needs. */

static void test_success(void)
{
    const char* in = "vgquery: point id=ups_load name=UPS负载 value=42.5 "
                     "unit=% ok=1 age_ms=900\n";
    const char* needles[] = {"UPS负载", "42.5", "%", "1 秒前"};

    expect_has(in, "successful sample", needles, 4);
}

/* 2. Staleness must be stated, not implied. */

static void test_stale(void)
{
    const char* in = "vgquery: point id=temp name=温度 value=23.1 unit=C "
                     "ok=1 age_ms=125000 stale=1\n";
    const char* needles[] = {"温度", "23.1", "过期"};

    expect_has(in, "stale sample", needles, 3);
}

/* 3. The value is echoed as produced: no rounding, no unit conversion. */

static void test_value_verbatim(void)
{
    const char* in = "vgquery: point id=ups_load name=UPS负载 value=42.1250 "
                     "unit=% ok=1 age_ms=500\n";
    const char* needles[] = {"42.1250"};

    expect_has(in, "value is verbatim", needles, 1);
}

/* 4. Cases that need wording keep going to the model. */

static void test_fall_through(void)
{
    expect_null("vgquery: reason=no_table msg=已确认点表不可读或尚未导入\n",
        "no table");
    expect_null("vgquery: reason=no_table msg=已确认点表为空\n",
        "empty table");
    expect_null("vgquery: reason=bad_arg msg=query 必须是字符串\n",
        "bad argument");
    expect_null("vgquery: reason=rate_limited msg=调用过于频繁\n",
        "rate limited");

    expect_null("vgquery: ambiguous n=3\nvgquery: candidate id=a name=A\n",
        "ambiguous result");

    /* Read failed: value is "-", so echoing it would state nothing useful. */

    expect_null("vgquery: point id=temp name=温度 value=- unit=C reason=read_failed "
                "age_ms=3000\n", "failed read");

    expect_null("vgquery: point id=temp name=温度 value=- unit=C reason=no_sample\n",
        "no sample");

    /* Missing fields must not produce a half-answer. */

    expect_null("vgquery: point id=temp name=温度 value=23\n",
        "truncated line");
    expect_null("vgquery: point id=temp name=温度 value=23 unit=C\n",
        "line without ok");
    expect_null("", "empty input");
    expect_null(NULL, "null input");
}

/* 5. A point name that is not resolvable uses the id, so the reply still
 *    names what was read. */

static void test_id_fallback(void)
{
    const char* in = "vgquery: point id=ups_load name=- value=7 unit=A "
                     "ok=1 age_ms=1000\n";
    const char* needles[] = {"ups_load", "7", "A"};

    expect_has(in, "id fallback", needles, 3);
}

int main(void)
{
    test_success();
    test_stale();
    test_value_verbatim();
    test_fall_through();
    test_id_fallback();

    if (s_failures != 0) {
        printf("%d failure(s)\n", s_failures);
        return 1;
    }

    printf("test_point_reply: all cases passed\n");
    return 0;
}
