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
 * tool_health_summary.c - 每日健康摘要工具
 *
 * 数据流（和 ui/health_summary.c 的卡片完全同源，只是不要求已经展示过）：
 *   pedometer_get_history_json()  -> 昨天/最近一天的步数
 *   hr_measure_history_snapshot() -> 最近 24h 心率的 min/max/count
 *   health_store_load("rest")     -> 昨夜 last_night.dur_s
 *   weather_cache_get()           -> 共享天气缓存
 *                         |
 *                         v
 *   health_summary_collect() + health_summary_text() + cJSON
 *
 * 工具层只做转手（和 tool_steps_history.c 一个写法），不碰存储/网络细节。
 * 没 UI 构建时 CMake 不编译 health_summary.c，此时给一个可读的错误 JSON。
 */

#include <nuttx/config.h>

#include "tools/tool_health_summary.h"
#include "agent_compat.h"

#include <stdio.h>

#if defined(CONFIG_AI_AGENT_LVGL_UI)
#include "ui/health_summary.h"
#endif

int tool_get_health_summary_execute(const char *input_json, char *output,
                                    size_t output_size)
{
    (void)input_json;

    if (output == NULL || output_size == 0)
      {
        return ERROR;
      }

    output[0] = '\0';

#if defined(CONFIG_AI_AGENT_LVGL_UI)
    /* 实现在 ui/health_summary.c（CMake 的 if(CONFIG_AI_AGENT_LVGL_UI) 分支）。
     * 失败时它已经在 output 里放了 {"error":"..."}。 */
    return health_summary_build_json(output, output_size) == 0 ? OK : ERROR;
#else
    snprintf(output, output_size,
             "{\"error\":\"health summary not enabled\"}");
    return ERROR;
#endif
}
