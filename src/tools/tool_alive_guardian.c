/*
 * Alive Guardian tool for the "Are You OK" application.
 *
 * The LLM only identifies the user's intent. All timing and cron
 * manipulation are completed locally in this tool so that a check-in
 * does not require multiple LLM -> tool -> LLM round trips.
 */

#include "tools/tool_alive_guardian.h"

#include "agent_compat.h"
#include "agent_config.h"
#include "infra/cron_service.h"

#include "cJSON.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define TAG "alive_guardian"

#define ALIVE_GUARDIAN_JOB_NAME "alive_guardian"
#define ALIVE_GUARDIAN_TEST_SECONDS 30U
#define ALIVE_GUARDIAN_FORMAL_SECONDS (72U * 60U * 60U)

#define ALIVE_GUARDIAN_TIMEOUT_MESSAGE \
    "安全守护已触发，请尽快确认用户当前状态。"

/*
 * Remove all existing Alive Guardian jobs.
 *
 * Normally there should be only one. Removing every matching job makes
 * repeated check-ins idempotent and prevents duplicate timeout tasks.
 */
static int alive_guardian_remove_existing(void)
{
    cron_job_t jobs[AGENT_CRON_MAX_JOBS];
    int count = cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS);
    int removed = 0;

    for (int i = 0; i < count; i++) {
        if (strcmp(jobs[i].name, ALIVE_GUARDIAN_JOB_NAME) != 0) {
            continue;
        }

        char id[AGENT_CRON_ID_LEN];
        memset(id, 0, sizeof(id));
        strncpy(id, jobs[i].id, sizeof(id) - 1);

        if (cron_remove_job(id) == OK) {
            removed++;
        }
    }

    return removed;
}

/*
 * Create a one-shot guard job.
 *
 * For the first QEMU/CLI verification the timeout notification is sent to
 * cli:console directly by cron_service. No second LLM request is required.
 */
static int alive_guardian_start(uint32_t delay_seconds,
                                const char *mode_name,
                                char *output,
                                size_t output_size)
{
    time_t now = time(NULL);

    /* Avoid creating a job against an obviously invalid clock. */
    if (now < 1704067200) { /* 2024-01-01 00:00:00 UTC */
        snprintf(output, output_size,
                 "错误：系统时间无效，无法启动安全守护。");
        return ERROR;
    }

    int removed = alive_guardian_remove_existing();

    cron_job_t job;
    memset(&job, 0, sizeof(job));

    strncpy(job.name,
            ALIVE_GUARDIAN_JOB_NAME,
            sizeof(job.name) - 1);

    job.kind = CRON_KIND_AT;
    job.at_epoch = (int64_t)now + (int64_t)delay_seconds;
    job.delete_after_run = true;

    strncpy(job.message,
            ALIVE_GUARDIAN_TIMEOUT_MESSAGE,
            sizeof(job.message) - 1);

    /*
     * Current verification target: NSH/CLI.
     * When QuickApp integration is added later, this destination can be
     * changed to AGENT_CHAN_QUICKAPP plus the appropriate chat_id.
     */
    strncpy(job.channel,
            AGENT_CHAN_CLI,
            sizeof(job.channel) - 1);

    strncpy(job.chat_id,
            "console",
            sizeof(job.chat_id) - 1);

    if (cron_add_job(&job) != OK) {
        snprintf(output, output_size,
                 "错误：创建安全守护任务失败。");
        return ERROR;
    }

    syslog(LOG_INFO,
           "[%s] guard started: mode=%s delay=%lu id=%s at=%lld "
           "removed_old=%d\n",
           TAG,
           mode_name,
           (unsigned long)delay_seconds,
           job.id,
           (long long)job.at_epoch,
           removed);

    if (delay_seconds == ALIVE_GUARDIAN_TEST_SECONDS) {
        snprintf(output, output_size,
                 "30秒安全守护已开启。任务ID=%s",
                 job.id);
    } else {
        snprintf(output, output_size,
                 "安全确认成功，72小时安全守护已重置。任务ID=%s",
                 job.id);
    }

    return OK;
}

static int alive_guardian_status(char *output, size_t output_size)
{
    cron_job_t jobs[AGENT_CRON_MAX_JOBS];
    int count = cron_list_jobs(jobs, AGENT_CRON_MAX_JOBS);
    time_t now = time(NULL);

    for (int i = 0; i < count; i++) {
        if (strcmp(jobs[i].name, ALIVE_GUARDIAN_JOB_NAME) != 0 ||
            !jobs[i].enabled) {
            continue;
        }

        int64_t remaining = jobs[i].next_run - (int64_t)now;
        if (remaining < 0) {
            remaining = 0;
        }

        snprintf(output, output_size,
                 "安全守护已开启。任务ID=%s，剩余约%lld秒。",
                 jobs[i].id,
                 (long long)remaining);
        return OK;
    }

    snprintf(output, output_size,
             "当前未开启安全守护。");
    return OK;
}

static int alive_guardian_cancel(char *output, size_t output_size)
{
    int removed = alive_guardian_remove_existing();

    if (removed > 0) {
        snprintf(output, output_size,
                 "安全守护已关闭。");
    } else {
        snprintf(output, output_size,
                 "当前未开启安全守护。");
    }

    return OK;
}

int tool_alive_guardian_execute(const char *input_json,
                                char *output,
                                size_t output_size)
{
    if (!input_json || !output || output_size == 0) {
        return ERROR;
    }

    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size,
                 "错误：Alive Guardian 参数不是有效 JSON。");
        return ERROR;
    }

    const char *action =
        cJSON_GetStringValue(cJSON_GetObjectItem(root, "action"));

    if (!action || action[0] == '\0') {
        snprintf(output, output_size,
                 "错误：缺少 action 参数。");
        cJSON_Delete(root);
        return ERROR;
    }

    int ret;

    if (strcmp(action, "test") == 0) {
        ret = alive_guardian_start(ALIVE_GUARDIAN_TEST_SECONDS,
                                   "test",
                                   output,
                                   output_size);
    } else if (strcmp(action, "confirm") == 0) {
        ret = alive_guardian_start(ALIVE_GUARDIAN_FORMAL_SECONDS,
                                   "formal",
                                   output,
                                   output_size);
    } else if (strcmp(action, "status") == 0) {
        ret = alive_guardian_status(output, output_size);
    } else if (strcmp(action, "cancel") == 0) {
        ret = alive_guardian_cancel(output, output_size);
    } else {
        snprintf(output, output_size,
                 "错误：未知 action '%s'。可用值：test、confirm、status、cancel。",
                 action);
        ret = ERROR;
    }

    cJSON_Delete(root);
    return ret;
}