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

#include "config_store.h"
#include "agent_config.h"
#include "agent_compat.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
#include "cJSON.h"

static const char *TAG = "cfgstore";

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;

/* ── helpers ─────────────────────────────────────────────────── */

static int mkdirs(const char *path)
{
    char tmp[256];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0700);
            *p = '/';
        }
    }
    mkdir(tmp, 0700);
    return OK;
}

static cJSON *load_json(void)
{
    FILE *f = fopen(AGENT_CONFIG_FILE, "r");
    if (!f) return cJSON_CreateObject();

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) { fclose(f); return cJSON_CreateObject(); }

    char *buf = (char *)malloc((size_t)(sz + 1));
    if (!buf) { fclose(f); return cJSON_CreateObject(); }

    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root) {
        return root;
    }

    /* Parse failure used to be silent, and the next claw_config_set()
     * would then merge into an empty object and save - wiping every other
     * key in the file. Make it loud. */
    syslog(LOG_ERR, "[%s] %s is not valid JSON (%ld bytes); "
           "starting from an empty object\n",
           TAG, AGENT_CONFIG_FILE, (long)sz);
    return cJSON_CreateObject();
}

#define AGENT_CONFIG_TMP AGENT_CONFIG_FILE ".tmp"

static int save_json(cJSON *root)
{
    char *str = cJSON_PrintUnformatted(root);
    if (!str) return ERROR;

    /* Write to a temp file and rename over the target: a partial write
     * (power cut, flash error) then leaves the previous config intact
     * instead of a truncated file. That truncation was observed on
     * hardware - config.json came back holding only the DNS keys after a
     * failed write, which silently dropped the LLM backend and API key.
     * 0600 on open() keeps the file owner-only (fopen's umask may not). */
    int fd = open(AGENT_CONFIG_TMP,
                  O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) { free(str); return ERROR; }

    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); unlink(AGENT_CONFIG_TMP); free(str); return ERROR; }

    int rc = OK;
    if (fputs(str, f) == EOF || fflush(f) != 0) {
        syslog(LOG_ERR, "[%s] Short write to %s: %d\n",
               TAG, AGENT_CONFIG_TMP, errno);
        rc = ERROR;
    }
    if (fclose(f) != 0) {
        rc = ERROR;
    }
    free(str);

    if (rc != OK) {
        unlink(AGENT_CONFIG_TMP);
        return ERROR;
    }

    if (rename(AGENT_CONFIG_TMP, AGENT_CONFIG_FILE) != 0) {
        syslog(LOG_ERR, "[%s] rename %s -> %s failed: %d\n",
               TAG, AGENT_CONFIG_TMP, AGENT_CONFIG_FILE, errno);
        unlink(AGENT_CONFIG_TMP);
        return ERROR;
    }
    return OK;
}

/* ── public API ──────────────────────────────────────────────── */

int config_store_init(void)
{
    mkdirs(AGENT_DATA_DIR);
    mkdirs(AGENT_CONFIG_DIR);
    mkdirs(AGENT_MEMORY_DIR);
    mkdirs(AGENT_SESSION_DIR);
    syslog(LOG_INFO, "[%s] Config store ready at %s\n", TAG, AGENT_CONFIG_FILE);
    return OK;
}

int claw_config_get(const char *key, char *buf, size_t buf_size)
{
    pthread_mutex_lock(&s_lock);
    cJSON *root = load_json();
    cJSON *item = cJSON_GetObjectItem(root, key);
    int ret = ERROR;
    if (item && cJSON_IsString(item) && item->valuestring[0] != '\0') {
        strncpy(buf, item->valuestring, buf_size - 1);
        buf[buf_size - 1] = '\0';
        ret = OK;
    }
    cJSON_Delete(root);
    pthread_mutex_unlock(&s_lock);
    return ret;
}

int claw_config_set(const char *key, const char *value)
{
    pthread_mutex_lock(&s_lock);
    cJSON *root = load_json();
    cJSON_DeleteItemFromObject(root, key);
    cJSON_AddStringToObject(root, key, value);
    int ret = save_json(root);
    cJSON_Delete(root);
    pthread_mutex_unlock(&s_lock);
    return ret;
}

int config_del(const char *key)
{
    pthread_mutex_lock(&s_lock);
    cJSON *root = load_json();
    cJSON_DeleteItemFromObject(root, key);
    int ret = save_json(root);
    cJSON_Delete(root);
    pthread_mutex_unlock(&s_lock);
    return ret;
}

int config_erase_all(void)
{
    pthread_mutex_lock(&s_lock);
    cJSON *empty = cJSON_CreateObject();
    int ret = save_json(empty);
    cJSON_Delete(empty);
    pthread_mutex_unlock(&s_lock);
    return ret;
}
