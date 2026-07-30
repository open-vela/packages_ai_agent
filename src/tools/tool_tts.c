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

#include "tools/tool_tts.h"
#include "agent_compat.h"
#include "voice/voice_channel.h"
#include "voice/voice_tts.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

#define TTS_TOOL_TEXT_MAX 600

int tool_tts_speak_execute(const char* input_json, char* output,
    size_t output_size)
{
    const char* backend;
    const cJSON* text;
    cJSON* request;
    cJSON* root;
    char* serialized;
    int ret;

    request = cJSON_Parse(input_json != NULL ? input_json : "{}");
    if (request == NULL) {
        snprintf(output, output_size, "{\"error\":\"invalid JSON\"}");
        return ERROR;
    }

    text = cJSON_GetObjectItemCaseSensitive(request, "text");
    if (!cJSON_IsString(text) || text->valuestring[0] == '\0') {
        cJSON_Delete(request);
        snprintf(output, output_size,
            "{\"error\":\"text must be a non-empty string\"}");
        return ERROR;
    }

    if (strlen(text->valuestring) > TTS_TOOL_TEXT_MAX) {
        cJSON_Delete(request);
        snprintf(output, output_size,
            "{\"error\":\"text exceeds %d bytes\"}",
            TTS_TOOL_TEXT_MAX);
        return ERROR;
    }

    backend = voice_tts_get_backend();
    if (backend == NULL) {
        cJSON_Delete(request);
        snprintf(output, output_size,
            "{\"error\":\"TTS backend is not configured\"}");
        return ERROR;
    }

    ret = voice_channel_speak(text->valuestring);
    cJSON_Delete(request);
    if (ret < 0) {
        snprintf(output, output_size,
            "{\"error\":\"TTS playback failed\",\"code\":%d}", ret);
        return ERROR;
    }

    root = cJSON_CreateObject();
    if (root == NULL) {
        snprintf(output, output_size, "{\"error\":\"out of memory\"}");
        return ERROR;
    }

    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddStringToObject(root, "backend", backend);
    serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized == NULL) {
        snprintf(output, output_size, "{\"error\":\"out of memory\"}");
        return ERROR;
    }

    snprintf(output, output_size, "%s", serialized);
    free(serialized);
    return OK;
}
