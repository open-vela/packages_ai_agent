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

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#include "tools/tool_cron.h"
#include "core/message_bus.h"
#include "infra/cron_service.h"
#include "agent_config.h"

#include "cJSON.h"
#include <errno.h>
#include <string.h>
#include <time.h>

/* -- Private Function Prototypes --------------------------------- */

static double get_numeric_value(cJSON* item, bool* ok);
static int parse_every_schedule(cJSON* root, cron_job_t* job,
    char* output, size_t output_size);
static int parse_at_schedule(cJSON* root, cron_job_t* job,
    char* output, size_t output_size);

/* -- Private Functions ------------------------------------------- */

/* Extract a numeric value from a cJSON item that may be either a
 * JSON number or a string containing digits. Many LLMs emit numeric
 * parameters as quoted strings; this helper tolerates both forms. */
static double get_numeric_value(cJSON* item, bool* ok)
{
    if (!item) {
        *ok = false;
        return 0.0;
    }

    if (cJSON_IsNumber(item)) {
        *ok = true;
        return item->valuedouble;
    }

    if (cJSON_IsString(item) && item->valuestring) {
        char* end;
        double v = strtod(item->valuestring, &end);

        if (end != item->valuestring && *end == '\0') {
            *ok = true;
            return v;
        }
    }

    *ok = false;
    return 0.0;
}

static int parse_every_schedule(cJSON* root, cron_job_t* job,
    char* output, size_t output_size)
{
    bool ok;
    double val = get_numeric_value(
        cJSON_GetObjectItem(root, "interval_s"), &ok);

    if (!ok || val <= 0) {
        snprintf(output, output_size,
            "Error: 'every' requires positive 'interval_s'");
        return ERROR;
    }

    job->kind = CRON_KIND_EVERY;
    job->interval_s = (uint32_t)val;
    job->delete_after_run = false;
    return OK;
}

static int parse_at_schedule(cJSON* root, cron_job_t* job,
    char* output, size_t output_size)
{
    bool ok;

    /* "at_epoch" is the canonical name, but some LLMs emit "target_epoch"
     * (possibly as a quoted digit string). get_numeric_value already accepts
     * both numbers and digit strings, so accept the alias here too. */
    cJSON* at_item = cJSON_GetObjectItem(root, "at_epoch");
    if (!at_item) {
        at_item = cJSON_GetObjectItem(root, "target_epoch");
    }

    double val = get_numeric_value(at_item, &ok);

    if (!ok || val <= 0) {
        snprintf(output, output_size,
            "Error: 'at' requires 'at_epoch' "
            "(unix timestamp as a number)");
        return ERROR;
    }

    job->kind = CRON_KIND_AT;
    job->at_epoch = (int64_t)val;

    time_t now = time(NULL);

    if (job->at_epoch <= now) {
        snprintf(output, output_size,
            "Error: at_epoch %lld is in the past (now=%lld)",
            (long long)job->at_epoch, (long long)now);
        return ERROR;
    }

    cJSON* delete_j = cJSON_GetObjectItem(root, "delete_after_run");
    job->delete_after_run = delete_j ? cJSON_IsTrue(delete_j) : true;
    return OK;
}

/* -- Public Functions -------------------------------------------- */

int tool_cron_add_execute(const char* input_json,
    char* output, size_t output_size)
{
    cJSON* root = cJSON_Parse(input_json);

    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    const char* name = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "name"));
    const char* schedule_type = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "schedule_type"));
    const char* message = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "message"));

    /* Some LLMs use "description" instead of "message" */
    if (!message) {
        message = cJSON_GetStringValue(
            cJSON_GetObjectItem(root, "description"));
    }

    if (!name || !schedule_type || !message) {
        snprintf(output, output_size,
            "Error: missing required fields. "
            "You MUST provide: name, schedule_type, message. "
            "Example: {\"name\":\"meeting_reminder\","
            "\"schedule_type\":\"at\","
            "\"at_epoch\":1773990900,"
            "\"message\":\"该开会了\"}. "
            "Call get_current_time first, then compute "
            "at_epoch = current_epoch + delay_seconds.");
        cJSON_Delete(root);
        return ERROR;
    }

    if (strlen(message) == 0) {
        snprintf(output, output_size,
            "Error: message must not be empty");
        cJSON_Delete(root);
        return ERROR;
    }

    cron_job_t job;
    memset(&job, 0, sizeof(job));
    strncpy(job.name, name, sizeof(job.name) - 1);
    strncpy(job.message, message, sizeof(job.message) - 1);

    const char* channel = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "channel"));
    const char* chat_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "chat_id"));

    if (channel) {
        strncpy(job.channel, channel, sizeof(job.channel) - 1);
    }

    if (chat_id) {
        strncpy(job.chat_id, chat_id, sizeof(job.chat_id) - 1);
    }

    const char* action = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "action"));
    const char* action_args = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "action_args"));

    if (action) {
        strncpy(job.action, action, sizeof(job.action) - 1);
    }

    if (action_args) {
        strncpy(job.action_args, action_args, sizeof(job.action_args) - 1);
    }

    const char* report_channel = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "report_channel"));
    const char* report_chat_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "report_chat_id"));

    if (report_channel) {
        strncpy(job.report_channel, report_channel, sizeof(job.report_channel) - 1);
    }

    if (report_chat_id) {
        strncpy(job.report_chat_id, report_chat_id, sizeof(job.report_chat_id) - 1);
    }

    if (strcmp(job.channel, AGENT_CHAN_FEISHU) == 0
        && (job.chat_id[0] == '\0'
            || strcmp(job.chat_id, "cron") == 0)) {
        snprintf(output, output_size,
            "Error: channel='feishu' requires a valid chat_id");
        cJSON_Delete(root);
        return ERROR;
    }

    int ret;

    if (strcmp(schedule_type, "every") == 0) {
        ret = parse_every_schedule(root, &job, output, output_size);
    } else if (strcmp(schedule_type, "at") == 0) {
        ret = parse_at_schedule(root, &job, output, output_size);
    } else {
        snprintf(output, output_size,
            "Error: schedule_type must be 'every' or 'at'");
        cJSON_Delete(root);
        return ERROR;
    }

    cJSON_Delete(root);

    if (ret != OK) {
        return ret;
    }

    int err = cron_add_job(&job);

    if (err != OK) {
        snprintf(output, output_size,
            "Error: failed to add job (%s)", strerror(errno));
        return err;
    }

    if (job.kind == CRON_KIND_EVERY) {
        snprintf(output, output_size,
            "OK: Added recurring job '%s' (id=%s), "
            "runs every %lu seconds. Next run at epoch %lld.",
            job.name, job.id,
            (unsigned long)job.interval_s,
            (long long)job.next_run);
    } else {
        snprintf(output, output_size,
            "OK: Added one-shot job '%s' (id=%s), "
            "fires at epoch %lld.%s",
            job.name, job.id,
            (long long)job.at_epoch,
            job.delete_after_run
                ? " Will be deleted after firing."
                : "");
    }

    syslog(LOG_INFO, "[tool_cron] cron_add: %s\n", output);
    return OK;
}

int tool_cron_list_execute(const char* input_json,
    char* output, size_t output_size)
{
    (void)input_json;

    /* cron_job_t is ~800 bytes; 16 of them on the stack would be ~12 KB, which
     * risks overflowing the 16 KB tool-thread stack. Allocate on the heap. */
    cron_job_t* jobs = malloc(sizeof(cron_job_t) * AGENT_CRON_MAX_JOBS);

    if (!jobs) {
        snprintf(output, output_size, "Error: out of memory");
        return ERROR;
    }

    int count = cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS);

    if (count == 0) {
        free(jobs);
        snprintf(output, output_size, "No cron jobs scheduled.");
        return OK;
    }

    size_t off = 0;

    off += snprintf(output + off, output_size - off,
        "Scheduled jobs (%d):\n", count);

    for (int i = 0; i < count && off < output_size - 1; i++) {
        const cron_job_t* j = &jobs[i];

        if (j->kind == CRON_KIND_EVERY) {
            off += snprintf(output + off, output_size - off,
                "  %d. [%s] \"%s\" every %lus, %s, "
                "next=%lld, last=%lld, ch=%s:%s\n",
                i + 1, j->id, j->name,
                (unsigned long)j->interval_s,
                j->enabled ? "enabled" : "disabled",
                (long long)j->next_run,
                (long long)j->last_run,
                j->channel, j->chat_id);
        } else {
            off += snprintf(output + off, output_size - off,
                "  %d. [%s] \"%s\" at %lld, %s, "
                "last=%lld, ch=%s:%s%s\n",
                i + 1, j->id, j->name,
                (long long)j->at_epoch,
                j->enabled ? "enabled" : "disabled",
                (long long)j->last_run,
                j->channel, j->chat_id,
                j->delete_after_run ? " (auto-delete)" : "");
        }
    }

    syslog(LOG_INFO, "[tool_cron] cron_list: %d jobs\n", count);
    free(jobs);
    return OK;
}

int tool_cron_remove_execute(const char* input_json,
    char* output, size_t output_size)
{
    cJSON* root = cJSON_Parse(input_json);

    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    const char* job_id = cJSON_GetStringValue(
        cJSON_GetObjectItem(root, "job_id"));

    if (!job_id || strlen(job_id) == 0) {
        snprintf(output, output_size, "Error: missing 'job_id' field");
        cJSON_Delete(root);
        return ERROR;
    }

    char id_copy[AGENT_CRON_ID_LEN];
    memset(id_copy, 0, sizeof(id_copy));
    strncpy(id_copy, job_id, sizeof(id_copy) - 1);
    cJSON_Delete(root);

    int err = cron_remove_job(id_copy);

    if (err == OK) {
        snprintf(output, output_size,
            "OK: Removed cron job %s", id_copy);
    } else {
        snprintf(output, output_size,
            "Error: job '%s' not found", id_copy);
    }

    syslog(LOG_INFO, "[tool_cron] cron_remove: %s\n", id_copy);
    return err;
}
