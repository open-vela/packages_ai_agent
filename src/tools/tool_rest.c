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

/* 构建：与 health/rest_track.c 同条件（CONFIG_AI_AGENT_LVGL_UI）。
 * 非 UI 构建返回 {"error":"rest tracker not enabled"}。 */

#include <nuttx/config.h>

#include "tools/tool_rest.h"
#include "agent_compat.h"

#include <string.h>
#include <syslog.h>

#include "cJSON.h"

/* 状态机/持久化都在 health/rest_track.c，这里只做 JSON 组装。 */
#include "health/rest_track.h"

#define TAG "[rest]"

#if defined(CONFIG_AI_AGENT_LVGL_UI)
static const char *rest_state_str(int state)
{
    switch (state) {
    case REST_STATE_QUIET: return "quiet";
    case REST_STATE_NIGHT: return "night";
    case REST_STATE_AWAKE: return "awake";
    default:               return "awake";
    }
}
#endif

int tool_get_rest_execute(const char *input_json, char *output,
                          size_t output_size)
{
    (void)input_json;

    if (output == NULL || output_size == 0) {
        return ERROR;
    }

#if defined(CONFIG_AI_AGENT_LVGL_UI)
    cJSON *root;
    cJSON *last_night;
    char *txt;
    uint32_t start;
    uint32_t end;
    uint32_t dur_s;
    uint32_t quiet_s;
    int state;
    int has_night;
    int ret;

    /* 页面可能还没打开过：这里懒启动，幂等。 */
    ret = rest_track_start();
    if (ret != 0) {
        syslog(LOG_WARNING, TAG " rest_track_start failed: %d\n", ret);
        snprintf(output, output_size,
                 "{\"error\":\"rest tracker start failed\"}");
        return ERROR;
    }

    state     = rest_track_get_state();
    quiet_s   = rest_track_current_quiet_s();
    has_night = rest_track_get_last_night(&start, &end, &dur_s);

    root = cJSON_CreateObject();
    if (root == NULL) {
        snprintf(output, output_size, "{\"error\":\"out of memory\"}");
        return ERROR;
    }

    cJSON_AddStringToObject(root, "state", rest_state_str(state));
    cJSON_AddNumberToObject(root, "quiet_s", (double)quiet_s);

    if (has_night == 1) {
        last_night = cJSON_CreateObject();
        if (last_night == NULL) {
            cJSON_Delete(root);
            snprintf(output, output_size, "{\"error\":\"out of memory\"}");
            return ERROR;
        }

        cJSON_AddNumberToObject(last_night, "start", (double)start);
        cJSON_AddNumberToObject(last_night, "end",   (double)end);
        cJSON_AddNumberToObject(last_night, "dur_s", (double)dur_s);
        cJSON_AddStringToObject(last_night, "estimate", "motion_only");
        cJSON_AddItemToObject(root, "last_night", last_night);
    } else {
        cJSON_AddNullToObject(root, "last_night");
    }

    cJSON_AddStringToObject(root, "note", "基于体动估算，非医疗级");

    txt = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (txt == NULL) {
        snprintf(output, output_size, "{\"error\":\"out of memory\"}");
        return ERROR;
    }

    /* 截断的 JSON 不合法，不如直接报错让调用方换大缓冲。 */
    if (strlen(txt) >= output_size) {
        free(txt);
        snprintf(output, output_size, "{\"error\":\"buffer too small\"}");
        return ERROR;
    }

    memcpy(output, txt, strlen(txt) + 1);
    free(txt);
    return OK;
#else
    snprintf(output, output_size,
             "{\"error\":\"rest tracker not enabled\"}");
    return ERROR;
#endif
}
