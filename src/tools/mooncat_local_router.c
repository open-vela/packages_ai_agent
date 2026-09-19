/* SPDX-License-Identifier: Apache-2.0 */

#include "mooncat_local_router.h"

#include "agent_compat.h"
#include "tools/tool_registry.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MOONCAT_ROUTER_JOB_NAME "mooncat-demo-once"
#define MOONCAT_ROUTER_NUMBER_SENTINEL UINT32_C(0x110000)

#define MOONCAT_ROUTER_ARGS \
    "{\"mode\":\"%s\",\"source\":\"simulated\",\"valid\":true," \
    "\"workout_active\":false,\"do_not_disturb\":false," \
    "\"inactivity_minutes\":75,\"sleep_debt_minutes\":40," \
    "\"stress_score\":72,\"battery_percent\":68}"

#define MOONCAT_ROUTER_ACTION_ARGS \
    "{\\\"mode\\\":\\\"execute\\\",\\\"source\\\":\\\"simulated\\\"," \
    "\\\"valid\\\":true,\\\"workout_active\\\":false," \
    "\\\"do_not_disturb\\\":false,\\\"inactivity_minutes\\\":75," \
    "\\\"sleep_debt_minutes\\\":40,\\\"stress_score\\\":72," \
    "\\\"battery_percent\\\":68}"

static uint32_t mooncat_mix32(uint32_t value, uint32_t item)
{
    value ^= item;
    return value * UINT32_C(16777619);
}

static uint32_t mooncat_hash_gram(uint32_t tag, uint32_t first,
                                  uint32_t second, bool has_second)
{
    uint32_t value = mooncat_mix32(UINT32_C(2166136261), tag);

    value = mooncat_mix32(value, first);
    if (has_second) {
        value = mooncat_mix32(value, second);
    }

    return value;
}

static uint32_t mooncat_utf8_next(const unsigned char **cursor,
                                  const unsigned char *end)
{
    const unsigned char *p = *cursor;
    uint32_t codepoint;
    size_t remaining = (size_t)(end - p);

    if (*p < 0x80) {
        *cursor = p + 1;
        return *p;
    }

    if (remaining >= 2 && (*p & 0xe0) == 0xc0 &&
        (p[1] & 0xc0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f);
        *cursor = p + 2;
        return codepoint >= 0x80 ? codepoint : UINT32_C(0xfffd);
    }

    if (remaining >= 3 && (*p & 0xf0) == 0xe0 &&
        (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x0f) << 12) |
                    ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f);
        *cursor = p + 3;
        return codepoint >= 0x800 &&
                       !(codepoint >= 0xd800 && codepoint <= 0xdfff)
                   ? codepoint
                   : UINT32_C(0xfffd);
    }

    if (remaining >= 4 && (*p & 0xf8) == 0xf0 &&
        (p[1] & 0xc0) == 0x80 &&
        (p[2] & 0xc0) == 0x80 && (p[3] & 0xc0) == 0x80) {
        codepoint = ((uint32_t)(p[0] & 0x07) << 18) |
                    ((uint32_t)(p[1] & 0x3f) << 12) |
                    ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f);
        *cursor = p + 4;
        return codepoint >= 0x10000 && codepoint <= 0x10ffff
                   ? codepoint
                   : UINT32_C(0xfffd);
    }

    *cursor = p + 1;
    return UINT32_C(0xfffd);
}

void mooncat_router_featurize(const char *text, uint8_t *features,
                              size_t feature_count)
{
    const unsigned char *cursor = (const unsigned char *)(text ? text : "");
    const unsigned char *end = cursor + strlen((const char *)cursor);
    uint32_t previous = 0;
    bool has_previous = false;
    bool previous_was_digit = false;
    size_t seen = 0;

    if (!features || feature_count == 0) {
        return;
    }

    memset(features, 0, feature_count);
    while (*cursor != '\0' && seen < 1024) {
        uint32_t codepoint = mooncat_utf8_next(&cursor, end);

        if (codepoint >= 'A' && codepoint <= 'Z') {
            codepoint += 'a' - 'A';
        }
        if (codepoint <= 0x20) {
            previous_was_digit = false;
            continue;
        }

        bool is_digit =
            (codepoint >= '0' && codepoint <= '9') ||
            (codepoint >= 0xff10 && codepoint <= 0xff19);
        if (is_digit) {
            if (previous_was_digit) {
                continue;
            }
            codepoint = MOONCAT_ROUTER_NUMBER_SENTINEL;
        }
        previous_was_digit = is_digit;

        features[mooncat_hash_gram(0x11, codepoint, 0, false) %
                 feature_count] = 1;
        if (has_previous) {
            features[mooncat_hash_gram(0x22, previous, codepoint, true) %
                     feature_count] = 1;
        }
        previous = codepoint;
        has_previous = true;
        seen++;
    }
}

static const char *mooncat_intent_name(enum mooncat_router_intent_e intent)
{
    switch (intent) {
    case MOONCAT_INTENT_COACH_NOW:
        return "coach_now";
    case MOONCAT_INTENT_COACH_PREVIEW:
        return "coach_preview";
    case MOONCAT_INTENT_COACH_SCHEDULE:
        return "coach_schedule";
    case MOONCAT_INTENT_COACH_LIST:
        return "coach_list";
    case MOONCAT_INTENT_FALLBACK:
    default:
        return "fallback";
    }
}

static char *mooncat_router_reply(
    const struct mooncat_router_prediction_s *prediction, const char *body)
{
    size_t size = strlen(body ? body : "") + 128;
    char *reply = calloc(1, size);

    if (reply) {
        snprintf(reply, size, "[LOCAL AI %u%% %s] %s",
                 (unsigned int)(prediction->confidence * 100.0f + 0.5f),
                 mooncat_intent_name(prediction->intent), body ? body : "");
    }
    return reply;
}

static unsigned int mooncat_parse_delay_seconds(const char *text)
{
    const char *marker = strstr(text, "CURRENT_EPOCH_PLUS_");

    if (marker) {
        const char *digits = marker + strlen("CURRENT_EPOCH_PLUS_");
        char *end = NULL;
        unsigned long value;

        errno = 0;
        value = strtoul(digits, &end, 10);
        if (end != digits && errno != ERANGE && value >= 10 &&
            value <= 86400 &&
            (*end == '\0' || *end == '"' ||
             isspace((unsigned char)*end))) {
            return (unsigned int)value;
        }
    }

    for (const char *p = text; *p != '\0'; p++) {
        char *end = NULL;
        unsigned long value;
        unsigned long multiplier = 0;

        if (!isdigit((unsigned char)*p)) {
            continue;
        }
        errno = 0;
        value = strtoul(p, &end, 10);
        if (errno == ERANGE) {
            p = end > p ? end - 1 : p;
            continue;
        }
        while (*end == ' ') {
            end++;
        }
        if (strncmp(end, "秒", strlen("秒")) == 0) {
            multiplier = 1;
        } else if (strncmp(end, "分钟", strlen("分钟")) == 0) {
            multiplier = 60;
        } else if (strncmp(end, "小时", strlen("小时")) == 0) {
            multiplier = 3600;
        }
        if (multiplier != 0 && value <= ULONG_MAX / multiplier) {
            value *= multiplier;
            if (value >= 10 && value <= 86400) {
                return (unsigned int)value;
            }
        }
        p = end > p ? end - 1 : p;
    }

    return 0;
}

static bool mooncat_parse_epoch(const char *time_output, long long *epoch)
{
    const char *marker = strstr(time_output, "UNIX epoch:");
    const char *digits;
    char *end = NULL;
    long long value;

    if (!marker) {
        return false;
    }

    digits = marker + strlen("UNIX epoch:");
    while (*digits == ' ') {
        digits++;
    }
    if (!isdigit((unsigned char)*digits)) {
        return false;
    }

    errno = 0;
    value = strtoll(digits, &end, 10);
    if (end == digits || errno == ERANGE || value <= 0) {
        return false;
    }
    while (isspace((unsigned char)*end)) {
        end++;
    }
    if (*end != '\0') {
        return false;
    }

    *epoch = value;
    return true;
}

static char *mooncat_execute_coach(
    const struct mooncat_router_prediction_s *prediction, const char *mode)
{
    char input[384];
    char output[1024] = "";

    snprintf(input, sizeof(input), MOONCAT_ROUTER_ARGS, mode);
    if (tool_registry_execute("mooncat_coach_tick", input, output,
                              sizeof(output)) != OK) {
        return mooncat_router_reply(
            prediction, "mooncat_coach_tick 失败，未完成本地请求。");
    }
    return mooncat_router_reply(prediction, output);
}

static char *mooncat_list_jobs(
    const struct mooncat_router_prediction_s *prediction)
{
    char output[2048] = "";

    if (tool_registry_execute("cron_list", "{}", output,
                              sizeof(output)) != OK) {
        return mooncat_router_reply(prediction,
                                    "cron_list 失败，未取得任务列表。");
    }
    return mooncat_router_reply(prediction, output);
}

static char *mooncat_schedule_coach(
    const char *text, const struct mooncat_router_prediction_s *prediction)
{
    char time_output[256] = "";
    char list_output[2048] = "";
    char input[1024];
    char add_output[1024] = "";
    long long epoch;
    unsigned int delay = mooncat_parse_delay_seconds(text);

    if (delay == 0) {
        return mooncat_router_reply(
            prediction, "缺少 10 秒到 24 小时的延迟，未创建任务。");
    }

    if (tool_registry_execute("get_current_time", "{}", time_output,
                              sizeof(time_output)) != OK) {
        return mooncat_router_reply(prediction,
                                    "获取当前时间失败，未创建任务。");
    }
    if (!mooncat_parse_epoch(time_output, &epoch) ||
        epoch > LLONG_MAX - (long long)delay) {
        return mooncat_router_reply(prediction,
                                    "当前时间解析失败，未创建任务。");
    }

    if (tool_registry_execute("cron_list", "{}", list_output,
                              sizeof(list_output)) != OK) {
        return mooncat_router_reply(prediction,
                                    "cron_list 失败，未创建任务。");
    }
    if (strstr(list_output, "\"" MOONCAT_ROUTER_JOB_NAME "\"") != NULL) {
        return mooncat_router_reply(
            prediction, "已存在 mooncat-demo-once，未重复创建。");
    }

    snprintf(input, sizeof(input),
             "{\"name\":\"" MOONCAT_ROUTER_JOB_NAME "\","
             "\"schedule_type\":\"at\",\"at_epoch\":%lld,"
             "\"message\":\"[DEMO] MoonCat proactive check completed\","
             "\"channel\":\"system\",\"chat_id\":\"mooncat-coach\","
             "\"action\":\"mooncat_coach_tick\","
             "\"action_args\":\"" MOONCAT_ROUTER_ACTION_ARGS "\"}",
             epoch + (long long)delay);
    if (tool_registry_execute("cron_add", input, add_output,
                              sizeof(add_output)) != OK) {
        return mooncat_router_reply(prediction,
                                    "cron_add 失败，未创建任务。");
    }
    return mooncat_router_reply(prediction, add_output);
}

char *mooncat_local_router_handle(const char *text)
{
    struct mooncat_router_prediction_s prediction;

    if (!text || mooncat_intent_model_predict(text, &prediction) != OK ||
        prediction.intent == MOONCAT_INTENT_FALLBACK) {
        return NULL;
    }

    switch (prediction.intent) {
    case MOONCAT_INTENT_COACH_NOW:
        return mooncat_execute_coach(&prediction, "execute");
    case MOONCAT_INTENT_COACH_PREVIEW:
        return mooncat_execute_coach(&prediction, "preview");
    case MOONCAT_INTENT_COACH_SCHEDULE:
        return mooncat_schedule_coach(text, &prediction);
    case MOONCAT_INTENT_COACH_LIST:
        return mooncat_list_jobs(&prediction);
    case MOONCAT_INTENT_FALLBACK:
    default:
        return NULL;
    }
}
