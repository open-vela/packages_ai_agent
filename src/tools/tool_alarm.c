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

/*
 * tool_alarm.c - Cloud tools that arm the on-device proactive alarm.
 *
 * Thin adapters over pet_care_alarm_set()/pet_care_alarm_cancel(): the care
 * scheduler owns the state machine (T-60s preview, T0 wake-up, escalation
 * when the user does not react, activity acknowledgement) and renders it on
 * the pet stage. Keeping the tools thin means the cloud path and the
 * on-device set_timer path share exactly one implementation.
 */

#include "tools/tool_alarm.h"
#include "agent_compat.h"

#include <cJSON.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#ifdef CONFIG_AI_AGENT_LVGL_UI
#include "ui/pet_care.h"
#endif

static const char *TAG = "tool_alarm";

/* Accept both numeric and string forms - models emit "10" and 10 alike. */
static int read_minutes(cJSON *root)
{
    cJSON *item = cJSON_GetObjectItem(root, "minutes");

    if (item == NULL) {
        return -1;
    }
    if (cJSON_IsNumber(item)) {
        return (int)item->valuedouble;
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return atoi(item->valuestring);
    }
    return -1;
}

int tool_set_alarm_execute(const char *input_json, char *output,
                           size_t output_size)
{
    cJSON *root;
    int minutes;

#ifdef CONFIG_AI_AGENT_LVGL_UI
    root = cJSON_Parse(input_json);
    if (root == NULL) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    minutes = read_minutes(root);
    cJSON_Delete(root);

    if (minutes <= 0 || minutes > 24 * 60) {
        snprintf(output, output_size,
            "Error: need \"minutes\" between 1 and 1440 (got %d). "
            "Example: {\"minutes\":10}", minutes);
        return ERROR;
    }

    if (pet_care_alarm_set(minutes) != 0) {
        snprintf(output, output_size,
            "Error: alarm not armed (scheduler rejected %d minutes)",
            minutes);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] alarm armed for %d min (cloud tool)\n",
           TAG, minutes);
    snprintf(output, output_size,
        "{\"status\":\"armed\",\"minutes\":%d,"
        "\"device_behavior\":\"preview 1 minute before, wake-up call on "
        "time, escalation if no reaction\"}", minutes);
    return OK;
#else
    (void)input_json;
    (void)minutes;
    snprintf(output, output_size,
        "Error: this build has no care scheduler");
    return ERROR;
#endif
}

int tool_cancel_alarm_execute(const char *input_json, char *output,
                              size_t output_size)
{
    (void)input_json;

#ifdef CONFIG_AI_AGENT_LVGL_UI
    pet_care_alarm_cancel();
    snprintf(output, output_size, "{\"status\":\"cancelled\"}");
    return OK;
#else
    snprintf(output, output_size,
        "Error: this build has no care scheduler");
    return ERROR;
#endif
}
