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

#include <nuttx/config.h>

#include "tools/tool_steps_history.h"
#include "agent_compat.h"

#include <string.h>
#include <syslog.h>

/* 数据在 health/pedometer.c: 落盘/跨天/7 天历史都在那边, 这里只做转手 ——
 * 和 tool_health.c 里 mic_selfcheck 的写法一样, 工具层不碰存储细节。 */
#if defined(CONFIG_AI_AGENT_LVGL_UI)
#include "health/pedometer.h"
#endif

#define TAG "[steps_history]"

int tool_get_steps_history_execute(const char *input_json, char *output,
                                   size_t output_size)
{
#if defined(CONFIG_AI_AGENT_LVGL_UI)
    int ret;
#endif

    (void)input_json;

    if (output == NULL || output_size == 0) {
        return ERROR;
    }

#if defined(CONFIG_AI_AGENT_LVGL_UI)
    /* pedometer.c 只在 UI 构建里编译 (见 CMakeLists.txt 的
     * if(CONFIG_AI_AGENT_LVGL_UI) 分支), 没计步器时给个可读的错误。 */
    ret = pedometer_get_history_json(output, output_size);
    if (ret < 0) {
        syslog(LOG_WARNING, TAG " build json failed: %d\n", ret);
        snprintf(output, output_size, "{\"error\":\"steps history unavailable\"}");
        return ERROR;
    }

    return OK;
#else
    /* 没有计步器: 仍然返回合法 JSON, 别让上层的 JSON 解析炸掉。 */
    snprintf(output, output_size, "{\"error\":\"pedometer not enabled\"}");
    return ERROR;
#endif
}
