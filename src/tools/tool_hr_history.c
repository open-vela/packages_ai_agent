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
 * tool_hr_history.c - 心率历史查询工具
 *
 * 数据流：
 *   hr_measure.c 的 288 条持久化环形缓冲
 *     -> hr_measure_history_json()（完整 JSON，内部互斥）
 *     -> 本工具裁掉旧条目，只保留最近 20 条
 *     -> 补上 latest_bpm / latest_ts / count 给 agent 用
 *
 * 为什么不直接给模型 288 条：JSON 约 7.5KB，既占上下文又对"最近趋势"
 * 没有帮助；20 条足够回答"最近心率怎么样"。
 */

#include "tools/tool_hr_history.h"
#include "agent_compat.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#include "health/hr_measure.h"

/* 工具输出里 items 的上限（完整历史最多 288 条）。 */
#define HR_HISTORY_ITEMS_MAX 20

static int hr_history_error(char *output, size_t output_size, const char *why)
{
    if (output != NULL && output_size > 0) {
        snprintf(output, output_size, "{\"error\":\"%s\"}", why);
    }
    return ERROR;
}

int tool_get_heartrate_history_execute(const char *input_json, char *output,
                                       size_t output_size)
{
    cJSON *root = NULL;
    cJSON *items;
    cJSON *items_out;
    cJSON *last;
    cJSON *jt;
    cJSON *jb;
    cJSON *out = NULL;
    char *raw;
    char *json;
    int total;
    int start;
    int i;

    (void)input_json;

    if (output == NULL || output_size == 0) {
        return ERROR;
    }
    output[0] = '\0';

    /* 完整历史由 hr_measure 统一序列化（拉锁 + cJSON），这里拿到的
     * 一定是一份完整一致的 {"items":[...]}。 */
    raw = (char *)malloc(HR_MEASURE_HISTORY_JSON_MAX);
    if (raw == NULL) {
        return hr_history_error(output, output_size, "out of memory");
    }

    if (hr_measure_history_json(raw, HR_MEASURE_HISTORY_JSON_MAX) != 0) {
        free(raw);
        return hr_history_error(output, output_size, "history unavailable");
    }

    root = cJSON_Parse(raw);
    free(raw);
    if (root == NULL) {
        return hr_history_error(output, output_size, "history parse failed");
    }

    items = cJSON_GetObjectItemCaseSensitive(root, "items");
    total = cJSON_IsArray(items) ? cJSON_GetArraySize(items) : 0;
    if (total <= 0) {
        cJSON_Delete(root);
        return hr_history_error(output, output_size,
                                "no heart rate history yet");
    }

    /* items 是旧 -> 新，最新一条在最后。 */
    last = cJSON_GetArrayItem(items, total - 1);
    jt = cJSON_GetObjectItemCaseSensitive(last, "t");
    jb = cJSON_GetObjectItemCaseSensitive(last, "bpm");

    out = cJSON_CreateObject();
    if (out == NULL) {
        cJSON_Delete(root);
        return hr_history_error(output, output_size, "out of memory");
    }

    cJSON_AddNumberToObject(out, "latest_bpm",
                            cJSON_IsNumber(jb) ? cJSON_GetNumberValue(jb) : 0);
    cJSON_AddNumberToObject(out, "latest_ts",
                            cJSON_IsNumber(jt) ? cJSON_GetNumberValue(jt) : 0);
    cJSON_AddNumberToObject(out, "count", (double)total);

    items_out = cJSON_AddArrayToObject(out, "items");
    if (items_out == NULL) {
        cJSON_Delete(out);
        cJSON_Delete(root);
        return hr_history_error(output, output_size, "out of memory");
    }

    start = (total > HR_HISTORY_ITEMS_MAX) ? total - HR_HISTORY_ITEMS_MAX : 0;
    for (i = start; i < total; i++) {
        cJSON *src = cJSON_GetArrayItem(items, i);
        cJSON *st = cJSON_GetObjectItemCaseSensitive(src, "t");
        cJSON *sb = cJSON_GetObjectItemCaseSensitive(src, "bpm");
        cJSON *obj = cJSON_CreateObject();

        if (obj == NULL) {
            cJSON_Delete(out);
            cJSON_Delete(root);
            return hr_history_error(output, output_size, "out of memory");
        }

        cJSON_AddNumberToObject(obj, "t",
                                cJSON_IsNumber(st) ? cJSON_GetNumberValue(st) : 0);
        cJSON_AddNumberToObject(obj, "bpm",
                                cJSON_IsNumber(sb) ? cJSON_GetNumberValue(sb) : 0);
        cJSON_AddItemToArray(items_out, obj);
    }

    cJSON_Delete(root);

    json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    if (json == NULL) {
        return hr_history_error(output, output_size, "out of memory");
    }

    if (strlen(json) >= output_size) {
        free(json);
        return hr_history_error(output, output_size, "output too small");
    }

    strcpy(output, json);
    free(json);
    return OK;
}
