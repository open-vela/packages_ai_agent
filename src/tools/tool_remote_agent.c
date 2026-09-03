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

#include "tools/tool_remote_agent.h"

#ifdef CONFIG_AI_AGENT_REMOTE_CTRL

#include "agent_config.h"
#include "channels/remote_ctrl_channel.h"

#include "cJSON.h"

#include <stdio.h>
#include <string.h>

static const char* TAG = "tool_remote";

int tool_remote_agent_prompt_execute(const char* input_json, char* output,
    size_t output_size)
{
    cJSON* root;
    const char* text;
    int ret;

    root = cJSON_Parse(input_json);
    if (root == NULL) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    text = cJSON_GetStringValue(cJSON_GetObjectItem(root, "text"));
    if (text == NULL || text[0] == '\0') {
        snprintf(output, output_size, "Error: 'text' is required");
        cJSON_Delete(root);
        return ERROR;
    }

    if (strlen(text) > REMOTE_PROMPT_TEXT_MAX) {
        snprintf(output, output_size,
            "Error: prompt exceeds %d bytes; send a shorter instruction",
            REMOTE_PROMPT_TEXT_MAX);
        cJSON_Delete(root);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] forwarding prompt (%u bytes)\n", TAG,
        (unsigned)strlen(text));

    /* The timeout is fixed rather than model-supplied: a large value would let
     * one tool call stall the whole local agent loop. */
    ret = remote_ctrl_channel_prompt(text, AGENT_REMOTE_CTRL_TURN_TIMEOUT_MS,
        output, output_size);

    cJSON_Delete(root);
    return ret;
}

int tool_remote_agent_status_execute(const char* input_json, char* output,
    size_t output_size)
{
    struct remote_status_s status;
    size_t used;

    (void)input_json;

    if (remote_ctrl_channel_status(&status) != OK) {
        snprintf(output, output_size,
            "Remote control is not configured on this device.");
        return OK;
    }

    used = (size_t)snprintf(output, output_size, "Remote agent: %s",
        remote_state_name(status.state));
    if (used >= output_size) {
        return OK;
    }

    if (status.summary[0] != '\0') {
        used += (size_t)snprintf(output + used, output_size - used, " (%s)",
            status.summary);
        if (used >= output_size) {
            return OK;
        }
    }

    if (status.billing[0] != '\0') {
        used += (size_t)snprintf(output + used, output_size - used, "\n%s",
            status.billing);
        if (used >= output_size) {
            return OK;
        }
    }

    /* Say plainly that this needs a person: the model has no tool for it and
     * should report back rather than keep retrying the prompt. */
    if (status.has_prompt) {
        used += (size_t)snprintf(output + used, output_size - used,
            "\nWaiting for a human to approve or deny '%s' on this device"
            "%s. You cannot answer this yourself; tell the user it is pending.",
            status.prompt.tool,
            status.decision_in_flight ? " (a decision was already sent)" : "");
        if (used >= output_size) {
            return OK;
        }
    }

    if (status.turn_active) {
        used += (size_t)snprintf(output + used, output_size - used,
            "\nA remote turn is currently running.");
        if (used >= output_size) {
            return OK;
        }
    }

    if (!status.chat_supported && status.state != REMOTE_STATE_OFFLINE) {
        snprintf(output + used, output_size - used,
            "\nThe remote bridge does not offer chat turns, so prompts cannot "
            "be sent.");
    }

    return OK;
}

#endif /* CONFIG_AI_AGENT_REMOTE_CTRL */
