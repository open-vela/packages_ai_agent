/*
 * Host regression test for the deterministic single-point reply.
 *
 * The shortcut exists to remove one model round-trip per point query.  What
 * must stay true:
 *
 *   1. A successful, fresh sample is echoed exactly: name, value, unit, age.
 *   2. A stale sample never reads as a current value; it falls through.
 *   3. Any shape that needs wording returns NULL so the LLM still handles
 *      it - ambiguity, no table, no sample, a failed read, partial lines.
 *   4. A question that asks why / whether / compare / report is not answered
 *      by echoing a value, even when one point resolves.
 *   5. The value is not rounded, rescaled or reformatted.
 *   6. Every fall-through names its reason, so a missed hit is visible.
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

static const char* DIRECT_QUESTION = "告诉我UPS负载的值";

static char* render(const char* input, const char* question,
    vg_point_reply_verdict_t* verdict)
{
    return vg_point_reply_render(input, question, verdict);
}

static void expect_null(const char* input, const char* what,
    vg_point_reply_verdict_t want)
{
    vg_point_reply_verdict_t got = VG_POINT_REPLY_HIT;
    char* out = render(input, DIRECT_QUESTION, &got);

    if (out != NULL) {
        printf("FAIL: %s should fall through, got: %s\n", what, out);
        s_failures++;
        free(out);
        return;
    }

    if (got != want) {
        printf("FAIL: %s reason %s, expected %s\n", what,
            vg_point_reply_verdict_name(got), vg_point_reply_verdict_name(want));
        s_failures++;
    }
}

static void expect_has(const char* input, const char* what,
    const char* needles[], int n)
{
    vg_point_reply_verdict_t got = VG_POINT_REPLY_NOT_TOOL_OUTPUT;
    char* out = render(input, DIRECT_QUESTION, &got);
    int i;

    if (out == NULL) {
        printf("FAIL: %s returned NULL (%s)\n", what,
            vg_point_reply_verdict_name(got));
        s_failures++;
        return;
    }

    if (got != VG_POINT_REPLY_HIT) {
        printf("FAIL: %s reported %s, expected hit\n", what,
            vg_point_reply_verdict_name(got));
        s_failures++;
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
    const char* needles[] = {"UPS负载", "42.5", "%", "1 秒前", "在线"};

    expect_has(in, "successful sample", needles, 5);
}

/* 2. Staleness must fall through: the model has to say how old the data is
 *    and what that means, an echo would present it as current. */

static void test_stale_falls_through(void)
{
    const char* in = "vgquery: point id=temp name=温度 value=23.1 unit=C "
                     "ok=1 age_ms=125000 stale=1\n";

    expect_null(in, "stale sample", VG_POINT_REPLY_STALE);
}

/* 3. The value is echoed as produced: no rounding, no unit conversion. */

static void test_value_verbatim(void)
{
    const char* in = "vgquery: point id=ups_load name=UPS负载 value=42.1250 "
                     "unit=% ok=1 age_ms=500\n";
    const char* needles[] = {"42.1250"};

    expect_has(in, "value is verbatim", needles, 1);
}

/* 4. Tool shapes that need wording keep going to the model. */

static void test_fall_through(void)
{
    expect_null("vgquery: reason=no_table msg=已确认点表不可读或尚未导入\n",
        "no table", VG_POINT_REPLY_NOT_TOOL_OUTPUT);
    expect_null("vgquery: reason=no_table msg=已确认点表为空\n",
        "empty table", VG_POINT_REPLY_NOT_TOOL_OUTPUT);
    expect_null("vgquery: reason=bad_arg msg=query 必须是字符串\n",
        "bad argument", VG_POINT_REPLY_NOT_TOOL_OUTPUT);
    expect_null("vgquery: reason=rate_limited msg=调用过于频繁\n",
        "rate limited", VG_POINT_REPLY_NOT_TOOL_OUTPUT);

    expect_null("vgquery: ambiguous n=3\nvgquery: candidate id=a name=A\n",
        "ambiguous result", VG_POINT_REPLY_AMBIGUOUS);

    expect_null("vgquery: table n=14\nvgquery: reason=not_found query=xyz\n",
        "not found", VG_POINT_REPLY_NOT_FOUND);

    /* Read failed: value is "-", so echoing it would state nothing useful. */

    expect_null("vgquery: point id=temp name=温度 value=- unit=C reason=read_failed "
                "age_ms=3000\n", "failed read", VG_POINT_REPLY_READ_FAILED);

    expect_null("vgquery: point id=temp name=温度 value=- unit=C reason=no_sample\n",
        "no sample", VG_POINT_REPLY_NO_SAMPLE);

    /* Missing fields must not produce a half-answer. */

    expect_null("vgquery: point id=temp name=温度 value=23\n",
        "truncated line", VG_POINT_REPLY_INCOMPLETE);
    expect_null("vgquery: point id=temp name=温度 value=23 unit=C\n",
        "line without ok", VG_POINT_REPLY_INCOMPLETE);
    expect_null("", "empty input", VG_POINT_REPLY_NOT_TOOL_OUTPUT);
    expect_null(NULL, "null input", VG_POINT_REPLY_NOT_TOOL_OUTPUT);

    /* A space inside a field would be cut in half by %s and echoed wrong:
     * unit "V AC" would render as "230 V".  The point table rejects such a
     * unit today, but the reply must not depend on that rule holding. */

    expect_null("vgquery: point id=volt name=A相电压 value=230 "
                "unit=V AC ok=1 age_ms=800\n",
        "space inside unit", VG_POINT_REPLY_INCOMPLETE);
    expect_null("vgquery: point id=volt name=A 相电压 value=230 "
                "unit=V ok=1 age_ms=800\n",
        "space inside name", VG_POINT_REPLY_INCOMPLETE);
}

/* 5. A fresh sample is not enough when the question asks for an explanation,
 *    a judgement or a report. */

static void test_explanation_questions(void)
{
    static const char* const questions[] = {
        "UPS负载为什么这么高",
        "UPS负载是不是不正常",
        "UPS负载正常吗",
        "解释一下UPS负载",
        "UPS负载的趋势如何",
        "帮我分析UPS负载",
        "UPS负载超过阈值了吗",
        "生成今天的UPS负载日报",
        "why is the UPS load high",
        "compare UPS load with last hour",
        NULL
    };
    const char* in = "vgquery: point id=ups_load name=UPS负载 value=42.5 "
                     "unit=% ok=1 age_ms=900\n";
    int i;

    for (i = 0; questions[i] != NULL; i++) {
        vg_point_reply_verdict_t got = VG_POINT_REPLY_HIT;
        char* out = render(in, questions[i], &got);

        if (out != NULL) {
            printf("FAIL: '%s' should reach the model, got: %s\n",
                questions[i], out);
            s_failures++;
            free(out);
            continue;
        }

        if (got != VG_POINT_REPLY_NOT_DIRECT) {
            printf("FAIL: '%s' reason %s, expected not_direct\n",
                questions[i], vg_point_reply_verdict_name(got));
            s_failures++;
        }
    }

    if (!vg_point_reply_is_direct_question(DIRECT_QUESTION)) {
        printf("FAIL: a plain value question must be direct\n");
        s_failures++;
    }
}

/* 5b. A question whose bytes are not valid UTF-8 cannot be matched against
 *     the keyword tables, so it must not take the shortcut: that is exactly
 *     what an explanation question looks like when a console sends it in the
 *     wrong code page. */

static void test_malformed_question(void)
{
    /* "为什么" in GBK.  Valid bytes, invalid UTF-8 as a whole. */
    static const char gbk[] = { (char)0xce, (char)0xaa, (char)0xca, (char)0xc3,
                                (char)0xb8, (char)0xba, (char)0x00 };
    const char* in = "vgquery: point id=ups_load name=UPS负载 value=42.5 "
                     "unit=% ok=1 age_ms=900\n";
    vg_point_reply_verdict_t got = VG_POINT_REPLY_HIT;
    char* out = render(in, gbk, &got);

    if (out != NULL) {
        printf("FAIL: malformed question must reach the model, got: %s\n", out);
        s_failures++;
        free(out);
    } else if (got != VG_POINT_REPLY_NOT_DIRECT) {
        printf("FAIL: malformed question reason %s, expected not_direct\n",
            vg_point_reply_verdict_name(got));
        s_failures++;
    }

    /* A truncated multibyte sequence at the end is malformed too.  Note
     * "\xe6\xb8\xa9" alone would be a complete 「温」; cut it after 2 bytes. */
    if (vg_point_reply_is_direct_question("\xe6\xb8")) {
        printf("FAIL: truncated UTF-8 must not be direct\n");
        s_failures++;
    }

    /* A valid CJK question still passes. */
    if (!vg_point_reply_is_direct_question("温度多少")) {
        printf("FAIL: valid CJK question must be direct\n");
        s_failures++;
    }
}

/* 6. A question that names several points cannot be answered with one
 *    value. */

static void test_multi_point_questions(void)
{
    static const char* const questions[] = {
        "温度和湿度分别是多少",
        "UPS负载和电池电量",
        "列出所有点位",
        "哪些点位在线",
        "temp and humidity",
        NULL
    };
    const char* in = "vgquery: point id=temp name=温度 value=23.1 unit=C "
                     "ok=1 age_ms=900\n";
    int i;

    for (i = 0; questions[i] != NULL; i++) {
        vg_point_reply_verdict_t got = VG_POINT_REPLY_HIT;
        char* out = render(in, questions[i], &got);

        if (out != NULL) {
            printf("FAIL: multi-point '%s' should reach the model, got: %s\n",
                questions[i], out);
            s_failures++;
            free(out);
            continue;
        }

        if (got != VG_POINT_REPLY_NOT_DIRECT) {
            printf("FAIL: multi-point '%s' reason %s, expected not_direct\n",
                questions[i], vg_point_reply_verdict_name(got));
            s_failures++;
        }
    }
}

/* 7. A point name that is not resolvable uses the id, so the reply still
 *    names what was read. */

static void test_id_fallback(void)
{
    const char* in = "vgquery: point id=ups_load name=- value=7 unit=A "
                     "ok=1 age_ms=1000\n";
    const char* needles[] = {"ups_load", "7", "A"};

    expect_has(in, "id fallback", needles, 3);
}

/* 8. A missing unit must not leave a stray space or a double blank. */

static void test_missing_unit(void)
{
    const char* in = "vgquery: point id=door name=门磁 value=0 unit=- "
                     "ok=1 age_ms=800\n";
    const char* needles[] = {"门磁", "0", "在线"};

    expect_has(in, "dash unit", needles, 3);
}

int main(void)
{
    test_success();
    test_stale_falls_through();
    test_value_verbatim();
    test_fall_through();
    test_explanation_questions();
    test_malformed_question();
    test_multi_point_questions();
    test_id_fallback();
    test_missing_unit();

    if (s_failures != 0) {
        printf("%d failure(s)\n", s_failures);
        return 1;
    }

    printf("test_point_reply: all cases passed\n");
    return 0;
}
