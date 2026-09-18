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
static cJSON *s_root;

/* Debugger-visible breadcrumbs; never contain configuration values. */
volatile unsigned int g_agent_config_stage;
volatile unsigned int g_agent_config_save_count;

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
    return root ? root : cJSON_CreateObject();
}

static int save_json(cJSON *root)
{
    g_agent_config_save_count++;
    g_agent_config_stage = 10;
    char *str = cJSON_PrintUnformatted(root);
    g_agent_config_stage = 11;
    if (!str) return ERROR;

    /* Use open() with explicit 0600 to ensure config file is owner-only.
     * fopen("w") inherits umask which may be too permissive. */
    g_agent_config_stage = 20;
    int fd = open(AGENT_CONFIG_FILE,
                  O_WRONLY | O_CREAT | O_TRUNC, 0600);
    g_agent_config_stage = 21;
    if (fd < 0) { free(str); return ERROR; }

    g_agent_config_stage = 30;
    FILE *f = fdopen(fd, "w");
    g_agent_config_stage = 31;
    if (!f) { close(fd); free(str); return ERROR; }
    g_agent_config_stage = 40;
    int written = fputs(str, f);
    g_agent_config_stage = 50;
    int closed = fclose(f);
    g_agent_config_stage = 60;
    free(str);
    g_agent_config_stage = 61;
    return written < 0 || closed != 0 ? ERROR : OK;
}

/* ── public API ──────────────────────────────────────────────── */

int config_store_init(void)
{
    mkdirs(AGENT_DATA_DIR);
    mkdirs(AGENT_CONFIG_DIR);
    mkdirs(AGENT_MEMORY_DIR);
    mkdirs(AGENT_SESSION_DIR);

    pthread_mutex_lock(&s_lock);
    if (s_root) {
        cJSON_Delete(s_root);
    }
    s_root = load_json();
    pthread_mutex_unlock(&s_lock);

    if (!s_root) {
        syslog(LOG_ERR, "[%s] Failed to initialize config cache\n", TAG);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Config store ready at %s\n", TAG, AGENT_CONFIG_FILE);
    return OK;
}

int claw_config_get(const char *key, char *buf, size_t buf_size)
{
    pthread_mutex_lock(&s_lock);
    cJSON *item = s_root ? cJSON_GetObjectItem(s_root, key) : NULL;
    int ret = ERROR;
    if (item && cJSON_IsString(item) && item->valuestring[0] != '\0') {
        strncpy(buf, item->valuestring, buf_size - 1);
        buf[buf_size - 1] = '\0';
        ret = OK;
    }
    pthread_mutex_unlock(&s_lock);
    return ret;
}

int claw_config_set(const char *key, const char *value)
{
    g_agent_config_stage = 1;
    pthread_mutex_lock(&s_lock);
    g_agent_config_stage = 2;
    if (!s_root) {
        s_root = cJSON_CreateObject();
    }
    g_agent_config_stage = 3;
    cJSON_DeleteItemFromObject(s_root, key);
    cJSON_AddStringToObject(s_root, key, value);
    g_agent_config_stage = 4;
    int ret = save_json(s_root);
    pthread_mutex_unlock(&s_lock);
    g_agent_config_stage = 70;
    syslog(LOG_DEBUG, "[config] save %u returned rc=%d\n",
            g_agent_config_save_count, ret);
    return ret;
}

int config_del(const char *key)
{
    pthread_mutex_lock(&s_lock);
    if (!s_root) {
        s_root = cJSON_CreateObject();
    }
    cJSON_DeleteItemFromObject(s_root, key);
    int ret = save_json(s_root);
    pthread_mutex_unlock(&s_lock);
    return ret;
}

int config_erase_all(void)
{
    pthread_mutex_lock(&s_lock);
    cJSON_Delete(s_root);
    s_root = cJSON_CreateObject();
    int ret = s_root ? save_json(s_root) : ERROR;
    pthread_mutex_unlock(&s_lock);
    return ret;
}
