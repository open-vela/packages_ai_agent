/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MoonCat active recovery coach tool adapter for openVela ai_agent.
 *
 * Milestone 1 accepts explicit simulated observations only.  The portable
 * policy is kept in common/mooncat_coach_policy.c; this file owns ai_agent
 * JSON parsing, process-local delivery cooldown and message-bus execution.
 */

#include "tools/tool_mooncat_coach.h"

#include "agent_compat.h"
#include "core/message_bus.h"
#include "mooncat_coach_policy.h"

#include "cJSON.h"
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MOONCAT_CHANNEL "mooncat"
#define MOONCAT_CHAT_ID "active-coach"

static pthread_mutex_t g_mooncat_cooldown_lock = PTHREAD_MUTEX_INITIALIZER;
static time_t g_mooncat_last_delivery_monotonic;

static const char *mooncat_priority_name(
    enum mooncat_coach_priority_e priority)
{
    switch (priority) {
    case MOONCAT_PRIORITY_GENTLE:
        return "gentle";
    case MOONCAT_PRIORITY_NORMAL:
        return "normal";
    case MOONCAT_PRIORITY_HIGH:
        return "high";
    case MOONCAT_PRIORITY_NONE:
    default:
        return "none";
    }
}

static const char *mooncat_source_name(enum mooncat_data_source_e source)
{
    switch (source) {
    case MOONCAT_DATA_SOURCE_SIMULATED:
        return "simulated";
    case MOONCAT_DATA_SOURCE_LIVE:
        return "live";
    case MOONCAT_DATA_SOURCE_UNSPECIFIED:
    default:
        return "unspecified";
    }
}

static bool mooncat_json_bool(cJSON *root, const char *name, bool *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);

    if (!cJSON_IsBool(item)) {
        return false;
    }

    *value = cJSON_IsTrue(item);
    return true;
}

static bool mooncat_json_uint(cJSON *root, const char *name,
                              unsigned int maximum,
                              unsigned int *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    double number;

    if (!cJSON_IsNumber(item)) {
        return false;
    }

    number = item->valuedouble;
    if (number < 0.0 || number > maximum || number != (double)(unsigned int)number) {
        return false;
    }

    *value = (unsigned int)number;
    return true;
}

static int mooncat_write_json(cJSON *root, char *output, size_t output_size)
{
    char *serialized;
    size_t length;

    serialized = cJSON_PrintUnformatted(root);
    if (!serialized) {
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"serialization_failed\"}");
        return ERROR;
    }

    length = strlen(serialized);
    if (length >= output_size) {
        free(serialized);
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"output_too_small\"}");
        return ERROR;
    }

    memcpy(output, serialized, length + 1);
    free(serialized);
    return OK;
}

static int mooncat_parse_observation(
    cJSON *root, struct mooncat_coach_observation_s *observation,
    const char **mode, char *error, size_t error_size)
{
    cJSON *source_item = cJSON_GetObjectItemCaseSensitive(root, "source");
    cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "mode");
    unsigned int number;

    if (!cJSON_IsString(source_item) || !source_item->valuestring ||
        !cJSON_IsString(mode_item) || !mode_item->valuestring) {
        snprintf(error, error_size, "source_and_mode_are_required");
        return ERROR;
    }

    if (strcmp(source_item->valuestring, "simulated") == 0) {
        observation->source = MOONCAT_DATA_SOURCE_SIMULATED;
    } else if (strcmp(source_item->valuestring, "live") == 0) {
        observation->source = MOONCAT_DATA_SOURCE_LIVE;
    } else {
        snprintf(error, error_size, "unsupported_source");
        return ERROR;
    }

    *mode = mode_item->valuestring;
    if (strcmp(*mode, "preview") != 0 && strcmp(*mode, "execute") != 0) {
        snprintf(error, error_size, "unsupported_mode");
        return ERROR;
    }

    if (!mooncat_json_bool(root, "valid", &observation->valid) ||
        !mooncat_json_bool(root, "workout_active",
                           &observation->workout_active) ||
        !mooncat_json_bool(root, "do_not_disturb",
                           &observation->do_not_disturb) ||
        !mooncat_json_uint(root, "inactivity_minutes", UINT16_MAX, &number)) {
        snprintf(error, error_size, "invalid_observation_fields");
        return ERROR;
    }
    observation->inactivity_minutes = (uint16_t)number;

    if (!mooncat_json_uint(root, "sleep_debt_minutes", UINT16_MAX, &number)) {
        snprintf(error, error_size, "invalid_sleep_debt_minutes");
        return ERROR;
    }
    observation->sleep_debt_minutes = (uint16_t)number;

    if (!mooncat_json_uint(root, "stress_score", 100, &number)) {
        snprintf(error, error_size, "invalid_stress_score");
        return ERROR;
    }
    observation->stress_score = (uint8_t)number;

    if (!mooncat_json_uint(root, "battery_percent", 100, &number)) {
        snprintf(error, error_size, "invalid_battery_percent");
        return ERROR;
    }
    observation->battery_percent = (uint8_t)number;

    return OK;
}

static time_t mooncat_monotonic_seconds(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }

    return now.tv_sec;
}

static uint32_t mooncat_cooldown_remaining(
    const struct mooncat_coach_decision_s *decision, time_t now)
{
    time_t elapsed;

    if (g_mooncat_last_delivery_monotonic <= 0 || now <= 0) {
        return 0;
    }

    if (now < g_mooncat_last_delivery_monotonic) {
        g_mooncat_last_delivery_monotonic = 0;
        return 0;
    }

    elapsed = now - g_mooncat_last_delivery_monotonic;
    if ((uint64_t)elapsed >= decision->cooldown_seconds) {
        return 0;
    }

    return decision->cooldown_seconds - (uint32_t)elapsed;
}

int tool_mooncat_coach_execute(const char *input_json, char *output,
                               size_t output_size)
{
    struct mooncat_coach_observation_s observation = {0};
    struct mooncat_coach_decision_s decision;
    cJSON *input;
    cJSON *result;
    const char *mode = NULL;
    char error[64] = "";
    char notification[192] = "";
    bool delivered = false;
    uint32_t remaining = 0;
    int status = OK;

    if (!output || output_size == 0) {
        return ERROR;
    }

    input = cJSON_Parse(input_json ? input_json : "{}");
    if (!input || !cJSON_IsObject(input)) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"invalid_json\"}");
        return ERROR;
    }

    if (mooncat_parse_observation(input, &observation, &mode,
                                  error, sizeof(error)) != OK) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"%s\"}", error);
        return ERROR;
    }

    decision = mooncat_coach_evaluate(&observation);
    if (decision.action != MOONCAT_COACH_NONE &&
        mooncat_coach_format_notification(&observation, &decision,
                                           notification,
                                           sizeof(notification)) != 0) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"notification_format_failed\"}");
        return ERROR;
    }

    if (decision.action != MOONCAT_COACH_NONE &&
        strcmp(mode, "execute") == 0) {
        agent_msg_t message = {0};
        time_t now = mooncat_monotonic_seconds();

        pthread_mutex_lock(&g_mooncat_cooldown_lock);
        if (now <= 0) {
            snprintf(error, sizeof(error), "monotonic_clock_unavailable");
            status = ERROR;
        } else {
            remaining = mooncat_cooldown_remaining(&decision, now);
        }
        if (status == OK && remaining == 0) {
            strncpy(message.channel, MOONCAT_CHANNEL,
                    sizeof(message.channel) - 1);
            strncpy(message.chat_id, MOONCAT_CHAT_ID,
                    sizeof(message.chat_id) - 1);
            message.content = strdup(notification);
            if (!message.content ||
                message_bus_push_outbound(&message) != OK) {
                free(message.content);
                message.content = NULL;
                snprintf(error, sizeof(error), "message_bus_push_failed");
                status = ERROR;
            } else {
                g_mooncat_last_delivery_monotonic = now;
                delivered = true;
            }
        }
        pthread_mutex_unlock(&g_mooncat_cooldown_lock);
    }

    result = cJSON_CreateObject();
    if (!result) {
        cJSON_Delete(input);
        snprintf(output, output_size,
                 "{\"ok\":false,\"error\":\"allocation_failed\"}");
        return ERROR;
    }

    cJSON_AddBoolToObject(result, "ok", status == OK);
    cJSON_AddStringToObject(result, "status",
        status != OK ? "error" :
        delivered ? "delivered" :
        (decision.action == MOONCAT_COACH_NONE || remaining > 0) ?
            "noop" : "preview");
    cJSON_AddStringToObject(result, "source",
                           mooncat_source_name(observation.source));
    cJSON_AddStringToObject(result, "evidence_boundary",
        observation.source == MOONCAT_DATA_SOURCE_SIMULATED ?
            "demo_only_not_physical_sensor" :
            "live_source_not_verified");
    cJSON_AddStringToObject(result, "mode", mode);
    cJSON_AddStringToObject(result, "action",
                           mooncat_coach_action_name(decision.action));
    cJSON_AddStringToObject(result, "priority",
                           mooncat_priority_name(decision.priority));
    cJSON_AddStringToObject(result, "reason_id",
                           remaining > 0 ? "cooldown_active" :
                           decision.reason_id);
    cJSON_AddStringToObject(result, "policy_reason_id", decision.reason_id);
    cJSON_AddNumberToObject(result, "duration_seconds",
                            decision.suggested_duration_seconds);
    cJSON_AddNumberToObject(result, "cooldown_seconds",
                            decision.cooldown_seconds);
    cJSON_AddNumberToObject(result, "cooldown_remaining_seconds", remaining);
    cJSON_AddBoolToObject(result, "delivered", delivered);
    cJSON_AddStringToObject(result, "channel", MOONCAT_CHANNEL);
    if (notification[0] != '\0') {
        cJSON_AddStringToObject(result, "notification", notification);
    }
    if (error[0] != '\0') {
        cJSON_AddStringToObject(result, "error", error);
    }

    cJSON_Delete(input);
    if (mooncat_write_json(result, output, output_size) != OK) {
        cJSON_Delete(result);
        return ERROR;
    }
    cJSON_Delete(result);
    return status;
}
