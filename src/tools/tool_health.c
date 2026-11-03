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

/* 麦克风自检 (health/mic_selfcheck.c): 工具层只做 JSON 转手 */
#include "health/mic_selfcheck.h"

/* 血氧单次测量 (health/spo2_measure.c): 异步起 worker 线程, 19s 采集,
 * 结束后自己上屏 + 语音播报, 不走工具返回值 */
#include "health/spo2_measure.h"

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


/* ── Microphone self test ────────────────────────────────────────
 *
 * "ask 麦克风自检" 的落点。实现在 health/mic_selfcheck.c, 这里只做
 * JSON 转手 —— 采集本身 3 秒阻塞, 工具在 agent 的 worker 线程上跑,
 * 不占 LVGL render 线程。
 */

int tool_mic_selfcheck_execute(const char *input_json, char *output,
                               size_t output_size)
{
    (void)input_json;
    return mic_selfcheck_run_json(output, output_size);
}

#ifdef CONFIG_AI_AGENT_LVGL_UI

/* max30102.c 暴露的最近一次血氧结果（flat build 跨库符号可见，
 * spo2_measure.c:38 也是这么 extern 的） */
extern float g_max30102_spo2_pct;
extern int   g_max30102_spo2_ok;

/* ── Blood oxygen (SpO2) ───────────────────────────────────────────
 *
 * "我血氧多少" 的落点。血氧没有 uORB 话题, 真实路径是 spo2_measure_start()
 * 异步起线程采 19 秒 —— 工具绝不能同步等它(会把 agent worker 卡住 20 秒):
 *
 *   有历史值   -> 直接报上次结果, 不再测;
 *   无历史值   -> 起一次测量, 由 spo2_measure.c 的 worker 在结束时
 *                 自己上屏 + 语音播报("您的血氧是 NN%"), 工具只回提示语;
 *   -EBUSY     -> 上一轮还在采, 提示稍等。
 *
 * 返回文本会被 TTS 直接念出来(也用 ui_font_cjk_18 上屏), 所以措辞口语、
 * 简短, 并提醒测量期间手指要贴稳传感器。
 */
int tool_get_spo2_execute(const char *input_json, char *output, size_t output_size)
{
    int ret;

    (void)input_json;

    /* 上次测量留有有效值: 直接报数(取整方式与 spo2_measure.c 上屏一致) */
    if (g_max30102_spo2_ok) {
        snprintf(output, output_size, "您上次测的血氧是%d%%",
                 (int)(g_max30102_spo2_pct + 0.5f));
        return OK;
    }

    /* 无有效历史值: 起一次异步测量, 结果稍后由 spo2_measure.c 播报 */
    ret = spo2_measure_start();
    if (ret == 0) {
        snprintf(output, output_size,
                 "血氧测量已开始，约20秒后播报结果，请把手指贴稳传感器");
        return OK;
    }

    if (ret == -EBUSY) {
        snprintf(output, output_size,
                 "上一次测量还在进行中，请把手指贴稳传感器");
        return OK;
    }

    syslog(LOG_WARNING, "[health] spo2_measure_start failed: %d\n", ret);
    snprintf(output, output_size, "血氧测量启动失败，请稍后再试");
    return ERROR;
}

#endif /* CONFIG_AI_AGENT_LVGL_UI */
