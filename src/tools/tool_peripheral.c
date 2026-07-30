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

#include "tools/tool_peripheral.h"
#include "agent_compat.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/param.h>
#include <sys/stat.h>

#include "cJSON.h"

struct peripheral_s {
    const char* name;
    const char* path;
};

static const struct peripheral_s g_peripherals[] = {
    { "audio_capture", "/dev/audio/pcm_in0" },
    { "audio_playback", "/dev/audio/pcm0" },
    { "camera", "/dev/video0" },
    { "display", "/dev/lcd0" },
    { "i2s", "/dev/i2schar0" },
};

int tool_peripheral_status_execute(const char* input_json, char* output,
    size_t output_size)
{
    cJSON* devices;
    cJSON* root;
    char* serialized;
    int available = 0;
    size_t i;

    (void)input_json;

    root = cJSON_CreateObject();
    devices = cJSON_CreateArray();
    if (root == NULL || devices == NULL) {
        cJSON_Delete(root);
        cJSON_Delete(devices);
        snprintf(output, output_size, "{\"error\":\"out of memory\"}");
        return ERROR;
    }

    for (i = 0; i < nitems(g_peripherals); i++) {
        struct stat status;
        cJSON* device = cJSON_CreateObject();
        int ret;
        int saved_errno;

        errno = 0;
        ret = stat(g_peripherals[i].path, &status);
        saved_errno = errno;

        cJSON_AddStringToObject(device, "name", g_peripherals[i].name);
        cJSON_AddStringToObject(device, "path", g_peripherals[i].path);
        cJSON_AddBoolToObject(device, "available", ret == 0);
        if (ret == 0) {
            available++;
        } else {
            cJSON_AddNumberToObject(device, "errno", saved_errno);
        }

        cJSON_AddItemToArray(devices, device);
    }

    cJSON_AddNumberToObject(root, "available_count", available);
    cJSON_AddNumberToObject(root, "checked_count", nitems(g_peripherals));
    cJSON_AddItemToObject(root, "devices", devices);

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
