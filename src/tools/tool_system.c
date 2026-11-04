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

#include "tools/tool_system.h"
#include "agent_compat.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include "cJSON.h"

#ifdef CONFIG_UORB
#include <uORB/uORB.h>
#include <system/state.h>
#endif

/* Helper: subscribe, copy once, unsubscribe */
#ifdef CONFIG_UORB
static int orb_read_once(const struct orb_metadata *meta, void *buf)
{
    int fd = orb_subscribe(meta);
    if (fd < 0) return -1;
    int ret = orb_copy(meta, fd, buf);
    orb_unsubscribe(fd);
    return ret;
}
#endif

int tool_get_battery_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

#ifdef CONFIG_UORB
    struct battery_state bs;
    memset(&bs, 0, sizeof(bs));
    if (orb_read_once(ORB_ID(battery_state), &bs) < 0) {
        snprintf(output, output_size, "{\"error\":\"sensor not available\"}");
        return ERROR;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "level",    bs.level);
    cJSON_AddNumberToObject(r, "charging", bs.state == 1 ? 1 : 0);
    cJSON_AddNumberToObject(r, "voltage",  bs.voltage);
    cJSON_AddNumberToObject(r, "temp",     bs.temp);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) { strncpy(output, s, output_size - 1); output[output_size-1] = '\0'; free(s); }
    return OK;
#else
    snprintf(output, output_size, "{\"error\":\"uORB not enabled\"}");
    return ERROR;
#endif
}

int tool_get_wear_state_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

#ifdef CONFIG_UORB
    struct wear_state ws;
    memset(&ws, 0, sizeof(ws));
    if (orb_read_once(ORB_ID(wear_state), &ws) < 0) {
        snprintf(output, output_size, "{\"error\":\"sensor not available\"}");
        return ERROR;
    }

    cJSON *r = cJSON_CreateObject();
    /* wear: 1=wore, 2=not wore */
    cJSON_AddBoolToObject(r, "worn", ws.wear == 1);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) { strncpy(output, s, output_size - 1); output[output_size-1] = '\0'; free(s); }
    return OK;
#else
    snprintf(output, output_size, "{\"error\":\"uORB not enabled\"}");
    return ERROR;
#endif
}

int tool_get_screen_state_execute(const char *input_json, char *output, size_t output_size)
{
    (void)input_json;

#ifdef CONFIG_UORB
    struct screen_onoff so;
    memset(&so, 0, sizeof(so));
    if (orb_read_once(ORB_ID(screen_onoff), &so) < 0) {
        snprintf(output, output_size, "{\"error\":\"sensor not available\"}");
        return ERROR;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "on", so.onoff == 1);
    char *s = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    if (s) { strncpy(output, s, output_size - 1); output[output_size-1] = '\0'; free(s); }
    return OK;
#else
    snprintf(output, output_size, "{\"error\":\"uORB not enabled\"}");
    return ERROR;
#endif
}

/* ── 待办事项工具 ──────────────────────────────────────────────────────
 * 把待办写入 /tmp/todo.txt（每行一条，最多 8 条），faced 信息轮播读取。
 * 用法：{"action":"add|list|clear","item":"..."}
 */

#define TODO_PATH        "/tmp/todo.txt"
#define TODO_MAX_ITEMS   8
#define TODO_MAX_LINE    80

int tool_todo_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root;
    cJSON *j;
    const char *action = NULL;
    const char *item = NULL;
    int fd;
    int nread;
    char buf[1024];
    int count = 0;
    size_t i;
    size_t jpos;

    if (!input_json || !output || output_size == 0) {
        return ERROR;
    }

    output[0] = '\0';
    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"error\":\"bad json\"}");
        return ERROR;
    }

    j = cJSON_GetObjectItemCaseSensitive(root, "action");
    if (j && cJSON_IsString(j) && j->valuestring) {
        action = j->valuestring;
    }

    j = cJSON_GetObjectItemCaseSensitive(root, "item");
    if (j && cJSON_IsString(j) && j->valuestring) {
        item = j->valuestring;
    }

    if (!action) {
        cJSON_Delete(root);
        snprintf(output, output_size, "{\"error\":\"action required\"}");
        return ERROR;
    }

    if (strcmp(action, "clear") == 0)
        {
            fd = open(TODO_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { close(fd); }
            cJSON_Delete(root);
            snprintf(output, output_size, "{\"ok\":true,\"result\":\"待办已清空\"}");
            return OK;
        }

    if (strcmp(action, "list") == 0)
        {
            cJSON *arr;

            fd = open(TODO_PATH, O_RDONLY);
            if (fd < 0) {
                cJSON_Delete(root);
                snprintf(output, output_size, "{\"ok\":true,\"todos\":[]}");
                return OK;
            }
            nread = read(fd, buf, sizeof(buf) - 1);
            close(fd);
            if (nread <= 0) {
                cJSON_Delete(root);
                snprintf(output, output_size, "{\"ok\":true,\"todos\":[]}");
                return OK;
            }
            buf[nread] = '\0';

            arr = cJSON_CreateArray();
            jpos = 0;
            while (jpos < (size_t)nread && arr != NULL)
                {
                    size_t eol;
                    char line[TODO_MAX_LINE];

                    eol = strcspn(buf + jpos, "\r\n");
                    if (eol > 0 && eol < sizeof(line))
                        {
                            memcpy(line, buf + jpos, eol);
                            line[eol] = '\0';
                            cJSON_AddItemToArray(arr, cJSON_CreateString(line));
                        }
                    jpos += eol;
                    while (jpos < (size_t)nread &&
                           (buf[jpos] == '\r' || buf[jpos] == '\n'))
                        {
                            jpos++;
                        }
                }

            if (arr) {
                char *s = cJSON_PrintUnformatted(arr);
                cJSON_Delete(arr);
                if (s) {
                    snprintf(output, output_size, "{\"ok\":true,\"todos\":%s}", s);
                    free(s);
                } else {
                    snprintf(output, output_size, "{\"ok\":true,\"todos\":[]}");
                }
            } else {
                snprintf(output, output_size, "{\"ok\":true,\"todos\":[]}");
            }
            cJSON_Delete(root);
            return OK;
        }

    if (strcmp(action, "add") == 0)
        {
            char item_clean[TODO_MAX_LINE];
            size_t len;
            char *nl;

            if (!item || item[0] == '\0') {
                cJSON_Delete(root);
                snprintf(output, output_size, "{\"error\":\"item required\"}");
                return ERROR;
            }

            /* 去首尾空白/换行，截断 */
            while (*item == ' ' || *item == '\t') { item++; }
            snprintf(item_clean, sizeof(item_clean), "%s", item);
            nl = strchr(item_clean, '\r');
            if (nl) { *nl = '\0'; }
            nl = strchr(item_clean, '\n');
            if (nl) { *nl = '\0'; }
            len = strlen(item_clean);
            while (len > 0 &&
                   (item_clean[len - 1] == ' ' || item_clean[len - 1] == '\t'))
                {
                    item_clean[--len] = '\0';
                }
            if (item_clean[0] == '\0') {
                cJSON_Delete(root);
                snprintf(output, output_size, "{\"error\":\"item empty\"}");
                return ERROR;
            }

            /* 读现有内容，统计条数 + 去重 */
            buf[0] = '\0';
            fd = open(TODO_PATH, O_RDONLY);
            if (fd >= 0) {
                nread = read(fd, buf, sizeof(buf) - 1);
                close(fd);
                if (nread > 0) {
                    buf[nread] = '\0';
                    for (i = 0; i < (size_t)nread; i++) {
                        if (buf[i] == '\n') { count++; }
                    }
                    if (strstr(buf, item_clean) != NULL) {
                        cJSON_Delete(root);
                        snprintf(output, output_size,
                                 "{\"ok\":true,\"result\":\"待办已存在：%s\"}",
                                 item_clean);
                        return OK;
                    }
                }
            }

            if (count >= TODO_MAX_ITEMS) {
                cJSON_Delete(root);
                snprintf(output, output_size,
                         "{\"error\":\"待办已达上限 %d 条\"}", TODO_MAX_ITEMS);
                return ERROR;
            }

            fd = open(TODO_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd < 0) {
                cJSON_Delete(root);
                snprintf(output, output_size, "{\"error\":\"cannot open todo\"}");
                return ERROR;
            }
            write(fd, item_clean, strlen(item_clean));
            write(fd, "\n", 1);
            close(fd);

            cJSON_Delete(root);
            snprintf(output, output_size, "{\"ok\":true,\"result\":\"已记下待办：%s\"}",
                     item_clean);
            return OK;
        }

    cJSON_Delete(root);
    snprintf(output, output_size, "{\"error\":\"unknown action\"}");
    return ERROR;
}

/* ── 语音控制舵机（转头/点头/摇头/回中） ────────────────────────────
 * 命令写入 /tmp/servo_cmd.txt，板端 servopoll 任务（bringup 常驻）读取并
 * 驱动 /dev/pwm0(pan)、/dev/pwm1(tilt)。执行后保持姿态数秒再恢复人脸跟随。
 *
 * 入参（LLM 自然语言路由到本工具后生成）：
 *   {"action":"center"}                        回中
 *   {"action":"shake"}                         摇头
 *   {"action":"pan","angle":30}                水平转到指定角度(10-170)
 *   {"action":"tilt","angle":60}               俯仰转到指定角度(10-170)
 *   {"action":"look","direction":"left|right|up|down"}  按方向看
 */

#define SERVO_CMD_PATH "/tmp/servo_cmd.txt"

int tool_servo_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root;
    const char *action;
    const char *direction;
    char cmd[64];

    root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "{\"error\":\"bad json\"}");
        return ERROR;
    }

    action = cJSON_GetObjectItem(root, "action") ?
             cJSON_GetObjectItem(root, "action")->valuestring : NULL;
    if (!action || action[0] == '\0') {
        cJSON_Delete(root);
        snprintf(output, output_size,
                 "{\"error\":\"action required: center|shake|pan|tilt|look\"}");
        return ERROR;
    }

    if (strcmp(action, "center") == 0)
        {
            snprintf(cmd, sizeof(cmd), "center");
        }
    else if (strcmp(action, "shake") == 0)
        {
            snprintf(cmd, sizeof(cmd), "shake");
        }
    else if (strcmp(action, "pan") == 0 || strcmp(action, "tilt") == 0)
        {
            int angle = 90;
            cJSON *a = cJSON_GetObjectItem(root, "angle");

            if (a && cJSON_IsNumber(a)) angle = a->valueint;
            if (angle < 10)  angle = 10;
            if (angle > 170) angle = 170;
            snprintf(cmd, sizeof(cmd), "%s=%d", action, angle);
        }
    else if (strcmp(action, "look") == 0)
        {
            direction = cJSON_GetObjectItem(root, "direction") ?
                        cJSON_GetObjectItem(root, "direction")->valuestring : NULL;
            if (!direction || direction[0] == '\0') {
                cJSON_Delete(root);
                snprintf(output, output_size,
                         "{\"error\":\"direction required for look\"}");
                return ERROR;
            }
            if      (strcmp(direction, "left")  == 0)
                snprintf(cmd, sizeof(cmd), "look_left");
            else if (strcmp(direction, "right") == 0)
                snprintf(cmd, sizeof(cmd), "look_right");
            else if (strcmp(direction, "up")    == 0)
                snprintf(cmd, sizeof(cmd), "look_up");
            else if (strcmp(direction, "down")  == 0)
                snprintf(cmd, sizeof(cmd), "look_down");
            else {
                cJSON_Delete(root);
                snprintf(output, output_size,
                         "{\"error\":\"direction must be left|right|up|down\"}");
                return ERROR;
            }
        }
    else
        {
            cJSON_Delete(root);
            snprintf(output, output_size, "{\"error\":\"unknown action\"}");
            return ERROR;
        }

    cJSON_Delete(root);

    int fd = open(SERVO_CMD_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        snprintf(output, output_size, "{\"error\":\"cannot open servo cmd\"}");
        return ERROR;
    }
    write(fd, cmd, strlen(cmd));
    write(fd, "\n", 1);
    close(fd);

    snprintf(output, output_size, "{\"ok\":true,\"result\":\"已执行：%s\"}", cmd);
    return OK;
}
