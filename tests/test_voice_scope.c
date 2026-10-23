/* SPDX-License-Identifier: Apache-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tools/voice_scope.h"
#include "cJSON.h"
cJSON *build_openai_tools_array(const char *tools_json);
int main(void)
{
    assert(vg_voice_scope_request("local_client", "velaguard:voice:42"));
    assert(!vg_voice_scope_request("local_client", "velaguard:42"));
    assert(!vg_voice_scope_request("local_client", "other:voice:42"));
    assert(!vg_voice_scope_request("system", "velaguard:voice:42"));
    const char *denied[] = {"run_shell", "read_file", "write_file", "vg_run_report",
        "vgpoint", "vg_point_read_extra", "VG_POINT_READ", "", NULL};
    for (int i = 0; i < 9; i++) assert(!vg_voice_scope_tool(denied[i]));
    assert(vg_voice_scope_tool("vg_point_read"));
    assert(vg_voice_scope_tool("vg_anomaly_read"));
    assert(vg_voice_scope_tool("vg_alarm_recovery"));
    const char *schema = "[{\"name\":\"run_shell\"},"
        "{\"name\":\"vg_point_read\",\"input_schema\":{\"type\":\"object\"}},"
        "{\"name\":\"write_file\"},"
        "{\"name\":\"vg_anomaly_read\"},"
        "{\"name\":\"vg_alarm_recovery\"},{}]";
    char *filtered = vg_voice_scope_schema(schema);
    assert(filtered && !strstr(filtered, "run_shell") && !strstr(filtered, "write_file"));
    cJSON *tools = cJSON_Parse(filtered);
    assert(cJSON_GetArraySize(tools) == 3 && strstr(filtered, "input_schema"));
    cJSON_Delete(tools);
    cJSON *wire = build_openai_tools_array(filtered);
    assert(wire && cJSON_GetArraySize(wire) == 3);
    cJSON *entry = cJSON_GetArrayItem(wire, 0);
    cJSON *fn = cJSON_GetObjectItem(entry, "function");
    assert(!strcmp(cJSON_GetObjectItem(fn, "name")->valuestring, "vg_point_read"));
    assert(cJSON_GetObjectItem(fn, "parameters"));
    cJSON_Delete(wire);
    free(filtered);
    assert(!vg_voice_scope_schema(NULL));
    assert(!vg_voice_scope_schema("bad"));
    assert(!vg_voice_scope_schema("{}"));
    filtered = vg_voice_scope_schema("[]");
    assert(filtered && !strcmp(filtered, "[]"));
    free(filtered);
    puts("test_voice_scope: OK (voice-only identity, exact allowlist, schema filtering, malformed input)");
    return 0;
}
