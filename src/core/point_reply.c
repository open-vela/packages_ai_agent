/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* strcasestr() is declared by <string.h> on glibc only with _DEFAULT_SOURCE.
 * NuttX provides it unconditionally, so the define is a no-op there. */

#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif

#include "core/point_reply.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* A question that asks for an explanation, a comparison, a judgement or a
 * report needs wording.  Echoing the value would answer a different question
 * than the one asked, so any of these terms sends the round back to the
 * model even when the tool resolved exactly one point. */

static const char* const s_explanation_terms[] = {
    "为什么", "为啥", "原因", "怎么会", "怎么回事", "怎么", "如何",
    "是否", "正常吗", "正常么", "不正常", "异常", "解释", "分析",
    "建议", "处理", "排查", "对比", "比较", "趋势", "变化", "历史",
    "告警", "阈值", "超过", "超标", "风险", "影响",
    "日报", "周报", "报告", "汇总", "统计",
    "why", "explain", "compare", "trend", "normal", "advice",
    "suggest", "threshold", "alarm", "report", "summary", "history",
    "analyz", "analysis",
    NULL
};

/* Several points in one question: the shortcut can render only one value, so
 * the model has to join the results. */

static const char* const s_multi_point_terms[] = {
    "、", "，", ",", "；", ";", "和", "与", "及", "以及", " and ",
    "分别", "各自", "全部", "所有", "每个", "哪些", "列出", "列表",
    NULL
};

static bool contains_any(const char* text, const char* const* terms)
{
    int i;

    for (i = 0; terms[i] != NULL; i++) {
        if (strcasestr(text, terms[i]) != NULL) {
            return true;
        }
    }
    return false;
}

bool vg_point_reply_is_direct_question(const char* question)
{
    if (question == NULL || question[0] == '\0') {
        return false;
    }

    if (contains_any(question, s_explanation_terms)) {
        return false;
    }

    if (contains_any(question, s_multi_point_terms)) {
        return false;
    }

    return true;
}

const char* vg_point_reply_verdict_name(vg_point_reply_verdict_t verdict)
{
    switch (verdict) {
    case VG_POINT_REPLY_HIT:
        return "hit";
    case VG_POINT_REPLY_AMBIGUOUS:
        return "ambiguous";
    case VG_POINT_REPLY_NOT_FOUND:
        return "not_found";
    case VG_POINT_REPLY_NOT_TOOL_OUTPUT:
        return "not_tool_output";
    case VG_POINT_REPLY_INCOMPLETE:
        return "incomplete";
    case VG_POINT_REPLY_READ_FAILED:
        return "read_failed";
    case VG_POINT_REPLY_NO_SAMPLE:
        return "no_sample";
    case VG_POINT_REPLY_STALE:
        return "stale";
    case VG_POINT_REPLY_NOT_DIRECT:
        return "not_direct";
    case VG_POINT_REPLY_RENDER_ERROR:
        return "render_error";
    default:
        return "?";
    }
}

char* vg_point_reply_render(const char* tool_output, const char* question,
    vg_point_reply_verdict_t* verdict)
{
    char id_buf[64], name_buf[96], value_buf[32], unit_buf[16];
    const char* unit;
    const char* age;
    char out[256];
    const char* id;
    const char* name;
    const char* value;
    const char* ok;
    unsigned long age_ms = 0;
    bool stale;
    int n;
    vg_point_reply_verdict_t why = VG_POINT_REPLY_HIT;

    if (verdict != NULL) {
        *verdict = VG_POINT_REPLY_HIT;
    }

    if (tool_output == NULL
        || strncmp(tool_output, "vgquery: point ", 15) != 0) {
        /* A short point name can legitimately match more than one entry, and
         * the tool answers that with a candidate list.  Naming those two
         * shapes separately keeps an ambiguous name from reading as a
         * malfunction in the round log. */

        if (tool_output != NULL) {
            if (strncmp(tool_output, "vgquery: ambiguous", 18) == 0) {
                why = VG_POINT_REPLY_AMBIGUOUS;
                goto fall_through;
            }

            if (strstr(tool_output, "reason=not_found") != NULL) {
                why = VG_POINT_REPLY_NOT_FOUND;
                goto fall_through;
            }
        }

        why = VG_POINT_REPLY_NOT_TOOL_OUTPUT;
        goto fall_through;
    }

    /* Every field must be present.  A reply built from a partial line would
     * state a value without saying how old it is, which is worse than the
     * extra round-trip. */

    id = strstr(tool_output, " id=");
    name = strstr(tool_output, " name=");
    value = strstr(tool_output, " value=");
    unit = strstr(tool_output, " unit=");
    ok = strstr(tool_output, " ok=");
    age = strstr(tool_output, " age_ms=");

    if (!id || !name || !value || !unit || !ok || !age) {
        if (strstr(tool_output, "reason=no_sample") != NULL) {
            why = VG_POINT_REPLY_NO_SAMPLE;
        } else if (strstr(tool_output, "reason=read_failed") != NULL) {
            why = VG_POINT_REPLY_READ_FAILED;
        } else {
            why = VG_POINT_REPLY_INCOMPLETE;
        }
        goto fall_through;
    }

    /* Only a successful read with a sample is echoed.  read_failed and
     * no_sample carry reasons the user needs explained, not repeated. */

    if (strncmp(ok, " ok=1", 5) != 0) {
        why = VG_POINT_REPLY_READ_FAILED;
        goto fall_through;
    }

    if (sscanf(id, " id=%63s", id_buf) != 1
        || sscanf(name, " name=%95s", name_buf) != 1
        || sscanf(value, " value=%31s", value_buf) != 1
        || sscanf(unit, " unit=%15s", unit_buf) != 1
        || sscanf(age, " age_ms=%lu", &age_ms) != 1) {
        why = VG_POINT_REPLY_INCOMPLETE;
        goto fall_through;
    }

    /* A stale sample must not read as a current value.  The full path states
     * how old the data is and what that means for the operator, so it keeps
     * the round. */

    stale = strstr(tool_output, " stale=1") != NULL;
    if (stale) {
        why = VG_POINT_REPLY_STALE;
        goto fall_through;
    }

    /* The question decides the shape of the answer, not only the tool
     * result: "why is the load high" resolves one point too, but it asks for
     * an explanation. */

    if (!vg_point_reply_is_direct_question(question)) {
        why = VG_POINT_REPLY_NOT_DIRECT;
        goto fall_through;
    }

    /* The value is echoed exactly as the tool produced it: no rounding, no
     * unit conversion, no recomputation.  This path removes a model
     * round-trip; it must not become a second source of truth. */

    n = snprintf(out, sizeof(out), "%s当前读数 %s %s（在线，%.0f 秒前采样）",
        strcmp(name_buf, "-") != 0 ? name_buf : id_buf,
        strcmp(value_buf, "-") != 0 ? value_buf : "无有效读数",
        strcmp(unit_buf, "-") != 0 ? unit_buf : "",
        (double)age_ms / 1000.0);

    if (n < 0 || (size_t)n >= sizeof(out)) {
        why = VG_POINT_REPLY_RENDER_ERROR;
        goto fall_through;
    }

    return strdup(out);

fall_through:
    if (verdict != NULL) {
        *verdict = why;
    }
    return NULL;
}

char* vg_point_reply_from_tool_output(const char* tool_output)
{
    return vg_point_reply_render(tool_output, NULL, NULL);
}
