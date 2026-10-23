/* SPDX-License-Identifier: Apache-2.0 */
#include "tools/voice_scope.h"
#include <string.h>
#include "cJSON.h"

bool vg_voice_scope_request(const char *channel, const char *chat_id)
{
#ifdef CONFIG_VG_HMI
    const char prefix[] = "velaguard:voice:";
    return channel && chat_id && strcmp(channel, "local_client") == 0
        && strncmp(chat_id, prefix, sizeof(prefix) - 1) == 0;
#else
    (void)channel;
    (void)chat_id;
    return false;
#endif
}

bool vg_voice_scope_tool(const char *name)
{
    return name && (strcmp(name, "vg_point_read") == 0
        || strcmp(name, "vg_anomaly_read") == 0
        || strcmp(name, "vg_alarm_recovery") == 0);
}

char *vg_voice_scope_schema(const char *all_tools)
{
    cJSON *tools = all_tools ? cJSON_Parse(all_tools) : NULL;
    if (!cJSON_IsArray(tools)) {
        cJSON_Delete(tools);
        return NULL;
    }
    for (int i = cJSON_GetArraySize(tools) - 1; i >= 0; i--) {
        cJSON *item = cJSON_GetArrayItem(tools, i);
        /* Registry format is flat; llm_parse adds the OpenAI wrapper. */
        cJSON *name = cJSON_GetObjectItemCaseSensitive(item, "name");
        if (!cJSON_IsString(name) || !vg_voice_scope_tool(name->valuestring)) {
            cJSON_DeleteItemFromArray(tools, i);
        }
    }
    char *result = cJSON_PrintUnformatted(tools);
    cJSON_Delete(tools);
    return result;
}

const char *vg_voice_scope_prompt(void)
{
    return "You are VelaGuard's read-only voice assistant. "
        "Call the requested tool once with the exact point ID supplied by the user: "
        "vg_point_read for readings, vg_anomaly_read for current anomalies, "
        "vg_alarm_recovery for recovery history. Never guess values or recovery. "
        "Do not access files, run commands, change configuration or control outputs. "
        "The device validates and renders the tool facts. No preamble or explanation.";
}

bool vg_voice_scope_model_options(cJSON *body, const char *prompt,
    const char *host, const char *model, bool controlled)
{
#ifdef CONFIG_VG_HMI
    if (!body || !controlled || !prompt || !host || !model ||
        strcmp(prompt, vg_voice_scope_prompt()) != 0 ||
        strcmp(model, "mimo-v2.6-flash") != 0 ||
        (strcmp(host, "token-plan-cn.xiaomimimo.com") != 0 &&
         strcmp(host, "api.xiaomimimo.com") != 0)) return false;
    /* MiMo API: thinking.type=disabled keeps ordinary tool calling. */
    cJSON *thinking = cJSON_CreateObject();
    if (!thinking) return false;
    cJSON_AddStringToObject(thinking, "type", "disabled");
    cJSON_AddItemToObject(body, "thinking", thinking);
    cJSON_DeleteItemFromObject(body, "max_tokens");
    cJSON_AddNumberToObject(body, "max_tokens", 256);
    return true;
#else
    (void)body; (void)prompt; (void)host; (void)model; (void)controlled;
    return false;
#endif
}
