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
        || strcmp(name, "vg_alarm_recovery") == 0
        || strcmp(name, "vg_run_report") == 0
        || strcmp(name, "vg_status_read") == 0);
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
    /* The product decision of 2026-10-25: the voice channel is a device
     * operations assistant, not a single-intent lookup.  The model picks and
     * combines the read-only tools itself and writes both the display answer
     * and the spoken summary; the board parses and bounds the result. */
    return "你是 VelaGuard 网关的设备运维助手，通过语音回答运维问题。"
        "可用只读工具：vg_point_read 查点位实时值，vg_anomaly_read 查当前全部异常，"
        "vg_alarm_recovery 查告警是否恢复，vg_run_report 查自上电运行报告，"
        "vg_status_read 查系统与连接概况。"
        "请先理解用户问题，再决定查什么：可以按需多次调用不同工具；"
        "问候闲聊可直接回应，但凡是涉及点位、数值、告警或系统状态的回答，"
        "必须先调用工具获取本机数据，没有查到就如实说明无法获取，不得凭记忆或推测给出数值；"
        "问题涉及多个相关点位时，把候选点位都查一遍，并列给出结果；"
        "结合上一轮问答理解追问。"
        "回答必须基于工具返回的真实数据，数据缺失或点位不存在时如实说明，不得编造数值；"
        "本助手只读，不能修改配置、清除告警或控制输出。"
        "用简体中文回答，先给结论再给数据依据；"
        "回答的最后一行必须是：播报：<不超过60字的口语化摘要，供语音播报，不要出现Markdown格式>";
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
    /* MiMo API: thinking.type=disabled keeps ordinary tool calling.  The
     * answer is now composed by the model (display text plus the 播报 summary
     * line), so the old 256-token cap only truncated it: 1024 leaves room for
     * a full list-style answer while staying cheap. */
    cJSON *thinking = cJSON_CreateObject();
    if (!thinking) return false;
    cJSON_AddStringToObject(thinking, "type", "disabled");
    cJSON_AddItemToObject(body, "thinking", thinking);
    cJSON_DeleteItemFromObject(body, "max_tokens");
    cJSON_AddNumberToObject(body, "max_tokens", 1024);
    cJSON_DeleteItemFromObject(body, "max_completion_tokens");
    cJSON_AddNumberToObject(body, "max_completion_tokens", 1024);
    return true;
#else
    (void)body; (void)prompt; (void)host; (void)model; (void)controlled;
    return false;
#endif
}
