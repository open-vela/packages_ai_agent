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

#include "infra/cron_service.h"
#include "core/message_bus.h"
#include "tools/tool_registry.h"
#include "agent_config.h"

#include "cJSON.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char* TAG = "cron";

#define MAX_CRON_JOBS AGENT_CRON_MAX_JOBS

/* -- Private Function Prototypes --------------------------------- */

static bool cron_sanitize_destination(cron_job_t* job);
static void cron_generate_id(char* id_buf);
static int cron_load_jobs(void);
static int cron_save_jobs(void);
static int cron_parse_job_item(cJSON* item, cron_job_t* job);
static void cron_fire_job(cron_job_t* job, time_t now);
static void cron_process_due_jobs(void);
static void compute_initial_next_run(cron_job_t* job);
static void* cron_task_main(void* arg);

/* -- Private Data ------------------------------------------------ */

static cron_job_t s_jobs[MAX_CRON_JOBS];
static int s_job_count;
static volatile bool s_cron_running;
static pthread_mutex_t s_cron_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cron_wake = PTHREAD_COND_INITIALIZER;

/* -- Private Functions ------------------------------------------- */

static bool cron_sanitize_destination(cron_job_t* job)
{
    bool changed = false;

    if (!job) {
        return false;
    }

    if (job->channel[0] == '\0') {
        strncpy(job->channel, AGENT_CHAN_SYSTEM, sizeof(job->channel) - 1);
        changed = true;
    }

    if (strcmp(job->channel, AGENT_CHAN_FEISHU) == 0) {
        if (job->chat_id[0] == '\0' || strcmp(job->chat_id, "cron") == 0) {
            syslog(LOG_WARNING,
                "[%s] Job %s: bad feishu chat_id, "
                "fallback to system:cron\n",
                TAG, job->id[0] ? job->id : "<new>");
            strncpy(job->channel, AGENT_CHAN_SYSTEM, sizeof(job->channel) - 1);
            strncpy(job->chat_id, "cron", sizeof(job->chat_id) - 1);
            changed = true;
        }
    } else if (job->chat_id[0] == '\0') {
        strncpy(job->chat_id, "cron", sizeof(job->chat_id) - 1);
        changed = true;
    }

    return changed;
}

static void cron_generate_id(char* id_buf)
{
    uint32_t r = ((uint32_t)rand());

    snprintf(id_buf, AGENT_CRON_ID_LEN, "%08x", (unsigned int)r);
}

/* Marker prefix prepended to a reminder destined for the on-device buddy when
 * the job requests a child-confirmation report. kid_buddy.c strips it before
 * display/TTS and uses the channel|chat_id to report back once the kid
 * acknowledges the reminder. */
#define KID_REPORT_TAG "[KID_REPORT:"

/* Build the reminder content, prefixing a confirmation-report directive when
 * the job asks for child-confirmation reporting back to the parent. Returns a
 * heap string (caller frees) or NULL on OOM. */
static char* cron_build_reminder_content(const cron_job_t* job)
{
    if (job->report_channel[0] == '\0' || job->report_chat_id[0] == '\0') {
        return strdup(job->message);
    }

    size_t n = strlen(KID_REPORT_TAG) + strlen(job->report_channel) + 1
             + strlen(job->report_chat_id) + 2
             + strlen(job->message) + 1;
    char* out = malloc(n);

    if (!out) {
        return NULL;
    }

    snprintf(out, n, "%s%s|%s]\n%s",
        KID_REPORT_TAG, job->report_channel, job->report_chat_id, job->message);
    return out;
}

/* Parse a single JSON object into a cron_job_t.
 * Returns OK on success, ERROR if the item should be skipped. */
static int cron_parse_job_item(cJSON* item, cron_job_t* job)
{
    const char* id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "id"));
    const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(item, "name"));
    const char* kind_str = cJSON_GetStringValue(cJSON_GetObjectItem(item, "kind"));
    const char* message = cJSON_GetStringValue(cJSON_GetObjectItem(item, "message"));
    const char* channel = cJSON_GetStringValue(cJSON_GetObjectItem(item, "channel"));
    const char* chat_id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "chat_id"));

    if (!id || !name || !kind_str || !message) {
        return ERROR;
    }

    memset(job, 0, sizeof(cron_job_t));
    strncpy(job->id, id, sizeof(job->id) - 1);
    strncpy(job->name, name, sizeof(job->name) - 1);
    strncpy(job->message, message, sizeof(job->message) - 1);
    strncpy(job->channel, channel ? channel : AGENT_CHAN_SYSTEM,
        sizeof(job->channel) - 1);
    strncpy(job->chat_id, chat_id ? chat_id : "cron", sizeof(job->chat_id) - 1);

    cJSON* enabled_j = cJSON_GetObjectItem(item, "enabled");
    job->enabled = enabled_j ? cJSON_IsTrue(enabled_j) : true;

    cJSON* delete_j = cJSON_GetObjectItem(item, "delete_after_run");
    job->delete_after_run = delete_j ? cJSON_IsTrue(delete_j) : false;

    if (strcmp(kind_str, "every") == 0) {
        job->kind = CRON_KIND_EVERY;
        cJSON* interval = cJSON_GetObjectItem(item, "interval_s");
        job->interval_s = (interval && cJSON_IsNumber(interval))
            ? (uint32_t)interval->valuedouble
            : 0;
    } else if (strcmp(kind_str, "at") == 0) {
        job->kind = CRON_KIND_AT;
        cJSON* at_epoch = cJSON_GetObjectItem(item, "at_epoch");
        job->at_epoch = (at_epoch && cJSON_IsNumber(at_epoch))
            ? (int64_t)at_epoch->valuedouble
            : 0;
    } else {
        return ERROR;
    }

    cJSON* last_run = cJSON_GetObjectItem(item, "last_run");
    job->last_run = (last_run && cJSON_IsNumber(last_run))
        ? (int64_t)last_run->valuedouble
        : 0;

    cJSON* next_run = cJSON_GetObjectItem(item, "next_run");
    job->next_run = (next_run && cJSON_IsNumber(next_run))
        ? (int64_t)next_run->valuedouble
        : 0;

    const char* action = cJSON_GetStringValue(cJSON_GetObjectItem(item, "action"));
    if (action) {
        strncpy(job->action, action, sizeof(job->action) - 1);
    }

    const char* action_args = cJSON_GetStringValue(cJSON_GetObjectItem(item, "action_args"));
    if (action_args) {
        strncpy(job->action_args, action_args, sizeof(job->action_args) - 1);
    }

    const char* report_channel = cJSON_GetStringValue(cJSON_GetObjectItem(item, "report_channel"));
    if (report_channel) {
        strncpy(job->report_channel, report_channel, sizeof(job->report_channel) - 1);
    }

    const char* report_chat_id = cJSON_GetStringValue(cJSON_GetObjectItem(item, "report_chat_id"));
    if (report_chat_id) {
        strncpy(job->report_chat_id, report_chat_id, sizeof(job->report_chat_id) - 1);
    }

    return OK;
}

/* -- Persistence ------------------------------------------------- */

static int cron_load_jobs(void)
{
    FILE* f = fopen(AGENT_CRON_FILE, "r");

    if (!f) {
        syslog(LOG_INFO, "[%s] No cron file, starting fresh\n", TAG);
        s_job_count = 0;
        return OK;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > AGENT_CRON_FILE_MAX_SIZE) {
        syslog(LOG_WARNING, "[%s] Cron file invalid size: %ld\n", TAG, fsize);
        fclose(f);
        s_job_count = 0;
        return OK;
    }

    char* buf = malloc(fsize + 1);

    if (!buf) {
        fclose(f);
        return ERROR;
    }

    size_t n = fread(buf, 1, fsize, f);
    buf[n] = '\0';
    fclose(f);

    cJSON* root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        syslog(LOG_WARNING, "[%s] Failed to parse cron JSON\n", TAG);
        s_job_count = 0;
        return OK;
    }

    cJSON* jobs_arr = cJSON_GetObjectItem(root, "jobs");

    if (!jobs_arr || !cJSON_IsArray(jobs_arr)) {
        cJSON_Delete(root);
        s_job_count = 0;
        return OK;
    }

    s_job_count = 0;
    bool repaired = false;
    cJSON* item;

    cJSON_ArrayForEach(item, jobs_arr)
    {
        if (s_job_count >= MAX_CRON_JOBS) {
            break;
        }

        cron_job_t* job = &s_jobs[s_job_count];

        if (cron_parse_job_item(item, job) != OK) {
            continue;
        }

        if (cron_sanitize_destination(job)) {
            repaired = true;
        }

        s_job_count++;
    }

    cJSON_Delete(root);
    if (repaired) {
        cron_save_jobs();
    }

    syslog(LOG_INFO, "[%s] Loaded %d cron jobs\n", TAG, s_job_count);
    return OK;
}

static int cron_save_jobs(void)
{
    cJSON* root = cJSON_CreateObject();
    cJSON* jobs_arr = cJSON_CreateArray();

    for (int i = 0; i < s_job_count; i++) {
        cron_job_t* job = &s_jobs[i];
        cJSON* item = cJSON_CreateObject();

        cJSON_AddStringToObject(item, "id", job->id);
        cJSON_AddStringToObject(item, "name", job->name);
        cJSON_AddBoolToObject(item, "enabled", job->enabled);
        cJSON_AddStringToObject(item, "kind",
            job->kind == CRON_KIND_EVERY ? "every" : "at");

        if (job->kind == CRON_KIND_EVERY) {
            cJSON_AddNumberToObject(item, "interval_s", job->interval_s);
        } else {
            cJSON_AddNumberToObject(item, "at_epoch", (double)job->at_epoch);
        }

        cJSON_AddStringToObject(item, "message", job->message);
        cJSON_AddStringToObject(item, "channel", job->channel);
        cJSON_AddStringToObject(item, "chat_id", job->chat_id);
        cJSON_AddNumberToObject(item, "last_run", (double)job->last_run);
        cJSON_AddNumberToObject(item, "next_run", (double)job->next_run);
        cJSON_AddBoolToObject(item, "delete_after_run", job->delete_after_run);

        if (job->action[0] != '\0') {
            cJSON_AddStringToObject(item, "action", job->action);
            cJSON_AddStringToObject(item, "action_args", job->action_args);
        }

        if (job->report_channel[0] != '\0') {
            cJSON_AddStringToObject(item, "report_channel", job->report_channel);
            cJSON_AddStringToObject(item, "report_chat_id", job->report_chat_id);
        }

        cJSON_AddItemToArray(jobs_arr, item);
    }

    cJSON_AddItemToObject(root, "jobs", jobs_arr);

    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (!json_str) {
        syslog(LOG_ERR, "[%s] Failed to serialize cron jobs\n", TAG);
        return ERROR;
    }

    FILE* f = fopen(AGENT_CRON_FILE, "w");

    if (!f) {
        syslog(LOG_ERR, "[%s] Cannot open %s for write\n", TAG, AGENT_CRON_FILE);
        free(json_str);
        return ERROR;
    }

    size_t len = strlen(json_str);
    size_t written = fwrite(json_str, 1, len, f);
    fclose(f);
    free(json_str);

    if (written != len) {
        syslog(LOG_ERR, "[%s] Cron save incomplete: %d/%d bytes\n", TAG,
            (int)written, (int)len);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Saved %d cron jobs\n", TAG, s_job_count);
    return OK;
}

/* -- Due-job processing ------------------------------------------ */

static void cron_fire_job(cron_job_t* job, time_t now)
{
    syslog(LOG_INFO, "[%s] Cron job firing: %s (%s)\n", TAG, job->name, job->id);

    if (job->action[0] != '\0') {
        /* Direct tool execution — no queue contention with agent loop */
        char tool_output[512];
        tool_output[0] = '\0';

        syslog(LOG_INFO, "[%s] Executing action: %s args=%.128s\n",
            TAG, job->action, job->action_args);

        int err = tool_registry_execute(job->action, job->action_args,
            tool_output, sizeof(tool_output));

        if (err != OK) {
            syslog(LOG_WARNING, "[%s] Action %s failed: %s\n",
                TAG, job->action, tool_output);
        } else {
            syslog(LOG_INFO, "[%s] Action %s OK: %.128s\n",
                TAG, job->action, tool_output);
        }

        /* Send a notification to the user about what happened.
         * Skip voice notification when the action itself failed —
         * speaking the failure message through voice channel can
         * trigger another round of audio close cascade errors. */
        if (err != OK
            && strcmp(job->channel, AGENT_CHAN_VOICE) == 0) {
            syslog(LOG_WARNING,
                "[%s] Skipping voice notification for failed action %s\n",
                TAG, job->action);
        } else if (job->channel[0] != '\0') {
            agent_msg_t notify;
            memset(&notify, 0, sizeof(notify));
            strncpy(notify.channel, job->channel, sizeof(notify.channel) - 1);
            strncpy(notify.chat_id, job->chat_id, sizeof(notify.chat_id) - 1);
            notify.content = strdup(job->message);
            if (notify.content) {
                if (message_bus_push_outbound(&notify) != OK) {
                    free(notify.content);
                }
            }
        }
    } else {
        /* Plain reminder — push directly to outbound. When the job requests a
         * confirmation report, the content carries a [KID_REPORT:...] prefix
         * that kid_buddy strips and uses to report the kid's acknowledgment. */
        agent_msg_t msg;
        memset(&msg, 0, sizeof(msg));
        strncpy(msg.channel, job->channel, sizeof(msg.channel) - 1);
        strncpy(msg.chat_id, job->chat_id, sizeof(msg.chat_id) - 1);
        msg.content = cron_build_reminder_content(job);

        if (msg.content) {
            int content_len = (int)strlen(msg.content);
            int err = message_bus_push_outbound(&msg);
            if (err != OK) {
                syslog(LOG_WARNING, "[%s] Failed to push cron message\n", TAG);
                free(msg.content);
            } else {
                syslog(LOG_INFO,
                    "[%s] Pushed reminder outbound ch=%s:%s len=%d\n",
                    TAG, msg.channel, msg.chat_id, content_len);
            }
        }
    }

    job->last_run = now;
}

static void cron_process_due_jobs(void)
{
    time_t now = time(NULL);
    bool changed = false;

    pthread_mutex_lock(&s_cron_lock);

    for (int i = 0; i < s_job_count; i++) {
        cron_job_t* job = &s_jobs[i];

        syslog(LOG_DEBUG,
            "[%s] Check job[%d] '%s': enabled=%d next_run=%lld now=%lld\n", TAG,
            i, job->name, job->enabled, (long long)job->next_run,
            (long long)now);

        if (!job->enabled || job->next_run <= 0 || job->next_run > now) {
            continue;
        }

        cron_fire_job(job, now);

        if (job->kind == CRON_KIND_AT) {
            if (job->delete_after_run) {
                syslog(LOG_INFO, "[%s] Deleting one-shot: %s\n", TAG, job->name);
                for (int j = i; j < s_job_count - 1; j++) {
                    s_jobs[j] = s_jobs[j + 1];
                }
                s_job_count--;
                i--;
            } else {
                job->enabled = false;
                job->next_run = 0;
            }
        } else {
            job->next_run = now + job->interval_s;
        }

        changed = true;
    }

    if (changed) {
        cron_save_jobs();
    }

    pthread_mutex_unlock(&s_cron_lock);
}

static void* cron_task_main(void* arg)
{
    (void)arg;

    while (s_cron_running) {
        /* Use timedwait instead of usleep so that cron_nudge() can
         * wake us immediately when a new job is added.  This also
         * survives NuttX deep-sleep better than plain usleep. */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += AGENT_CRON_CHECK_INTERVAL_MS / 1000;

        pthread_mutex_lock(&s_cron_lock);
        pthread_cond_timedwait(&s_cron_wake, &s_cron_lock, &ts);
        pthread_mutex_unlock(&s_cron_lock);

        cron_process_due_jobs();
    }

    return NULL;
}

static void compute_initial_next_run(cron_job_t* job)
{
    time_t now = time(NULL);

    if (job->kind == CRON_KIND_EVERY) {
        job->next_run = now + job->interval_s;
    } else if (job->kind == CRON_KIND_AT) {
        if (job->at_epoch > now) {
            job->next_run = job->at_epoch;
        } else {
            job->next_run = 0;
            job->enabled = false;
        }
    }
}

/* -- Public Functions -------------------------------------------- */

int cron_service_init(void) { return cron_load_jobs(); }

int cron_service_start(void)
{
    pthread_mutex_lock(&s_cron_lock);

    if (s_cron_running) {
        pthread_mutex_unlock(&s_cron_lock);
        syslog(LOG_WARNING, "[%s] Cron task already running\n", TAG);
        return OK;
    }

    time_t now = time(NULL);
    bool changed = false;

    for (int i = 0; i < s_job_count; i++) {
        cron_job_t* job = &s_jobs[i];

        if (!job->enabled) {
            continue;
        }

        /* Stale job: next_run already set but in the past.
         * interval_s is relative to the previous action completion
         * (e.g. music playback end), so rescheduling to now+interval
         * would produce wrong timing.  Disable instead. */
        if (job->next_run > 0 && job->next_run <= now) {
            syslog(LOG_WARNING,
                "[%s] disabling stale job: %s (next_run %lld <= now %lld)\n",
                TAG, job->name, (long long)job->next_run, (long long)now);
            job->enabled = false;
            job->next_run = 0;
            changed = true;
            continue;
        }

        if (job->next_run <= 0) {
            if (job->kind == CRON_KIND_EVERY) {
                job->next_run = now + job->interval_s;
            } else if (job->kind == CRON_KIND_AT && job->at_epoch > now) {
                job->next_run = job->at_epoch;
            }
        }
    }

    if (changed) {
        cron_save_jobs();
    }

    s_cron_running = true;
    pthread_mutex_unlock(&s_cron_lock);

    int err = agent_task_create(cron_task_main, "cron", AGENT_CRON_STACK,
        NULL, AGENT_CRON_PRIO);

    if (err != OK) {
        s_cron_running = false;
        syslog(LOG_ERR, "[%s] Failed to create cron task\n", TAG);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Cron started (%d jobs, interval %ds)\n", TAG,
        s_job_count, AGENT_CRON_CHECK_INTERVAL_MS / 1000);
    return OK;
}

void cron_service_stop(void)
{
    if (s_cron_running) {
        s_cron_running = false;
        syslog(LOG_INFO, "[%s] Cron service stopping\n", TAG);
    }
}

int cron_add_job(cron_job_t* job)
{
    pthread_mutex_lock(&s_cron_lock);

    if (s_job_count >= MAX_CRON_JOBS) {
        syslog(LOG_WARNING, "[%s] Max cron jobs reached (%d)\n", TAG,
            MAX_CRON_JOBS);
        pthread_mutex_unlock(&s_cron_lock);
        return ERROR;
    }

    cron_generate_id(job->id);
    cron_sanitize_destination(job);

    job->enabled = true;
    job->last_run = 0;
    compute_initial_next_run(job);

    s_jobs[s_job_count] = *job;
    s_job_count++;

    cron_save_jobs();

    /* Wake the cron thread so it re-evaluates with the new job */
    pthread_cond_signal(&s_cron_wake);

    pthread_mutex_unlock(&s_cron_lock);

    syslog(LOG_INFO,
        "[%s] Added job: %s (%s) kind=%s "
        "next=%lld ch=%s:%s\n",
        TAG, job->name, job->id, job->kind == CRON_KIND_EVERY ? "every" : "at",
        (long long)job->next_run, job->channel, job->chat_id);
    return OK;
}

int cron_remove_job(const char* job_id)
{
    pthread_mutex_lock(&s_cron_lock);

    for (int i = 0; i < s_job_count; i++) {
        if (strcmp(s_jobs[i].id, job_id) == 0) {
            syslog(LOG_INFO, "[%s] Removing job: %s (%s)\n", TAG, s_jobs[i].name,
                job_id);

            for (int j = i; j < s_job_count - 1; j++) {
                s_jobs[j] = s_jobs[j + 1];
            }
            s_job_count--;

            cron_save_jobs();
            pthread_mutex_unlock(&s_cron_lock);
            return OK;
        }
    }

    pthread_mutex_unlock(&s_cron_lock);
    syslog(LOG_WARNING, "[%s] Cron job not found: %s\n", TAG, job_id);
    return ERROR;
}

int cron_list_jobs(cron_job_t* out, int max_count)
{
    int count;

    pthread_mutex_lock(&s_cron_lock);
    count = s_job_count < max_count ? s_job_count : max_count;
    memcpy(out, s_jobs, count * sizeof(cron_job_t));
    pthread_mutex_unlock(&s_cron_lock);

    return count;
}
