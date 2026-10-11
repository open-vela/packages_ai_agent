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

#include "tools/tool_health.h"
#include "agent_compat.h"

#include <string.h>
#include <syslog.h>

#include "cJSON.h"

#if defined(CONFIG_UORB) && defined(CONFIG_FITNESS_ALGO)
#include <uORB/uORB.h>
#if __has_include(<fitness/algo_heartrate.h>) && __has_include(<fitness/algo_pedometer.h>)
#define AGENT_HAS_FITNESS_TOPICS 1
#include <fitness/algo_heartrate.h>
#include <fitness/algo_pedometer.h>
#endif
#endif

#if defined(CONFIG_UORB) && defined(CONFIG_FITNESS_ALGO)
#include <poll.h>

/* 读一次话题。**必须先等一个新的更新再 copy**: 刚 subscribe 时 copy 会拿不到
 * 数据(返回 -EAGAIN), 这就是 get_heartrate 一直报 sensor not available 的原因 ——
 * 话题其实存在(/dev/uorb/algo_heartrate0), 发布端 100Hz。
 * 这里 poll 最多 ~500ms (50 x 10ms) 等一次更新, 失败时打日志说明原因。 */
static int orb_read_once(const struct orb_metadata *meta, void *buf)
{
    struct pollfd pfd;
    int fd;
    int ret = -1;
    int i;

    fd = orb_subscribe(meta);
    if (fd < 0) {
        syslog(LOG_WARNING, "[health] orb_subscribe(%s) failed: %d\n",
               meta->o_name, fd);
        return -1;
    }

    pfd.fd = fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    for (i = 0; i < 50; i++) {
        ret = poll(&pfd, 1, 10);
        if (ret > 0) {
            ret = orb_copy(meta, fd, buf);
            if (ret == 0) break;
        }
    }

    if (ret != 0) {
        syslog(LOG_WARNING, "[health] no update on %s after 500ms (ret=%d)\n",
               meta->o_name, ret);
    }

    orb_unsubscribe(fd);
    return (ret == 0) ? 0 : -1;
}
#endif

int tool_get_heartrate_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

#if defined(CONFIG_UORB) && defined(CONFIG_FITNESS_ALGO)
    struct algo_heartrate hr;
    memset(&hr, 0, sizeof(hr));
    if (orb_read_once(ORB_ID(algo_heartrate), &hr) < 0) {
        snprintf(output, output_size, "{\"error\":\"sensor not available\"}");
        return ERROR;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "bpm", (double)hr.bpm);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) { strncpy(output, s, output_size - 1); output[output_size-1] = '\0'; free(s); }
    return OK;
#else
    snprintf(output, output_size, "{\"error\":\"fitness topics not available\"}");
    return ERROR;
#endif
}

int tool_get_steps_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

#if defined(CONFIG_UORB) && defined(CONFIG_FITNESS_ALGO)
    struct algo_pedometer pd;
    memset(&pd, 0, sizeof(pd));
    if (orb_read_once(ORB_ID(algo_pedometer), &pd) < 0) {
        snprintf(output, output_size, "{\"error\":\"sensor not available\"}");
        return ERROR;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "steps",          (double)pd.count);
    cJSON_AddNumberToObject(r, "step_frequency", (double)pd.step_frequency);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) { strncpy(output, s, output_size - 1); output[output_size-1] = '\0'; free(s); }
    return OK;
#else
    snprintf(output, output_size, "{\"error\":\"fitness topics not available\"}");
    return ERROR;
#endif
}
