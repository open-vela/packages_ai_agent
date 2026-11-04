/****************************************************************************
 * tool_persona.c - set_persona 工具：把模型的切换请求落到 persona 模块。
 *
 * 工具层只做三件事：解析 input_json、调用 persona_set()、把当前人格名回给
 * 模型。持久化/节流/降级全在 persona.c 里，NSH 的 persona 命令也走同一
 * 套逻辑，避免两份实现行为不一致。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include "tools/tool_persona.h"
#include "ui/persona.h"

#include <cJSON.h>

static const char *TAG = "tool_persona";

int tool_set_persona_execute(const char *input_json, char *output,
                             size_t output_size)
{
    cJSON *root;
    cJSON *item;
    persona_id_t id;
    persona_id_t current;

    root = cJSON_Parse(input_json);
    if (root == NULL) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    item = cJSON_GetObjectItem(root, "persona");
    if (item == NULL || !cJSON_IsString(item) || item->valuestring == NULL) {
        cJSON_Delete(root);
        snprintf(output, output_size,
                 "Error: missing \"persona\" "
                 "(gentle|sassy|cheer|温柔|毒舌|元气)");
        return ERROR;
    }

    if (persona_parse_name(item->valuestring, &id) != OK) {
        snprintf(output, output_size,
                 "Error: unknown persona \"%s\"; "
                 "use gentle/sassy/cheer or 温柔/毒舌/元气",
                 item->valuestring);
        cJSON_Delete(root);
        return ERROR;
    }
    cJSON_Delete(root);

    if (persona_set(id) != OK) {
        snprintf(output, output_size, "Error: cannot switch persona");
        return ERROR;
    }

    current = persona_get();
    syslog(LOG_INFO, "[%s] persona switched to %s (%d) via tool\n",
           TAG, persona_name(current), (int)current);
    snprintf(output, output_size,
             "{\"status\":\"ok\",\"persona\":\"%s\",\"id\":%d}",
             persona_name(current), (int)current);
    return OK;
}
