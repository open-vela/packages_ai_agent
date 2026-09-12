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

#pragma once
/**
 * cron_service.h — Cron scheduler for AI Agent (Vela/NuttX port)
 *
 */

#include "agent_compat.h"
#include <stdbool.h>
#include <stdint.h>

/* Schedule types */
typedef enum {
    CRON_KIND_EVERY = 0,   /* Recurring interval in seconds */
    CRON_KIND_AT    = 1,   /* One-shot at unix timestamp */
} cron_kind_t;

/* A single cron job */
typedef struct {
    char id[9];            /* 8-char hex ID + null */
    char name[32];
    bool enabled;
    cron_kind_t kind;
    uint32_t interval_s;   /* For EVERY: interval in seconds */
    int64_t at_epoch;      /* For AT: unix timestamp */
    char message[256];     /* Message text (reminder) or fallback */
    char channel[16];      /* Reply channel (default "system") */
    char chat_id[64];      /* Reply chat_id (default "cron") */
    int64_t last_run;      /* Last run epoch */
    int64_t next_run;      /* Next run epoch */
    bool delete_after_run; /* Remove job after firing (for AT jobs) */
    char action[32];       /* Tool name to execute directly (empty = send message) */
    char action_args[256]; /* JSON args for the tool action */
    char report_channel[16]; /* If non-empty, report child confirmation to this
                                channel after the reminder is acknowledged (e.g.
                                "mqtt" to notify the parent). */
    char report_chat_id[64]; /* Destination chat_id for the confirmation report */
} cron_job_t;

/**
 * Initialize the cron service. Loads jobs from persistent storage.
 */
int cron_service_init(void);

/**
 * Start the cron timer thread. Call after network is connected and
 * time is synced.
 */
int cron_service_start(void);

/**
 * Stop the cron timer thread.
 */
void cron_service_stop(void);

/**
 * Add a new cron job.
 * @param job  Pointer to job struct (id will be generated)
 * @return OK on success, ERROR if max jobs reached
 */
int cron_add_job(cron_job_t *job);

/**
 * Remove a cron job by ID.
 * @param job_id  8-char job ID
 * @return OK on success, ERROR if not found
 */
int cron_remove_job(const char *job_id);

/**
 * Copy all cron jobs into caller-provided buffer.
 * @param out        Caller buffer to receive job copies
 * @param max_count  Maximum entries to copy
 * @return Number of jobs actually copied
 */
int cron_list_jobs(cron_job_t *out, int max_count);
