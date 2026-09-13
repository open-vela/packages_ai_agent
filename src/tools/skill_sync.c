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

/**
 * skill_sync.c — Pull skills from Feishu Bitable to local filesystem
 *
 * Flow:
 *   1. Refresh tenant_access_token (reuses feishu_http.c)
 *   2. GET bitable records (enabled=true, paginated)
 *   3. Filter by target_device
 *   4. Compare version with local .versions.json
 *   5. Write changed skills to /data/ai_agent/skills/
 *   6. Optionally clean orphaned local skills (full mode)
 */

#include "tools/skill_sync.h"

#if AGENT_SKILL_SYNC_ENABLED

#include "channels/feishu_internal.h"
#include "infra/config_store.h"
#include "agent_config.h"
#include "cJSON.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "skill_sync";

/* ── Local version tracking ────────────────────────────────────── */

/** Simple in-memory version map: skill_id → version string */
typedef struct {
    char skill_id[64];
    char version[32];
} version_entry_t;

typedef struct {
    version_entry_t entries[128];
    int count;
} version_map_t;

static void version_map_load(version_map_t *map)
{
    map->count = 0;

    FILE *f = fopen(AGENT_SKILL_SYNC_VERSION_FILE, "r");
    if (!f) {
        return;
    }

    /* Read entire file (small, ~2KB max) */
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 8192) {
        fclose(f);
        return;
    }

    char *buf = malloc((size_t)fsize + 1);
    if (!buf) {
        fclose(f);
        return;
    }

    fread(buf, 1, (size_t)fsize, f);
    buf[fsize] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root || !cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return;
    }

    cJSON *item = NULL;
    cJSON_ArrayForEach(item, root) {
        if (map->count >= 128) {
            break;
        }
        if (cJSON_IsString(item) && item->string) {
            strncpy(map->entries[map->count].skill_id, item->string,
                    sizeof(map->entries[0].skill_id) - 1);
            strncpy(map->entries[map->count].version, item->valuestring,
                    sizeof(map->entries[0].version) - 1);
            map->count++;
        }
    }

    cJSON_Delete(root);
    syslog(LOG_DEBUG, "[%s] Loaded %d version entries\n", TAG, map->count);
}

static const char *version_map_get(const version_map_t *map,
                                   const char *skill_id)
{
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].skill_id, skill_id) == 0) {
            return map->entries[i].version;
        }
    }
    return NULL;
}

static void version_map_set(version_map_t *map, const char *skill_id,
                            const char *version)
{
    /* Update existing */
    for (int i = 0; i < map->count; i++) {
        if (strcmp(map->entries[i].skill_id, skill_id) == 0) {
            strncpy(map->entries[i].version, version,
                    sizeof(map->entries[0].version) - 1);
            return;
        }
    }

    /* Add new */
    if (map->count < 128) {
        strncpy(map->entries[map->count].skill_id, skill_id,
                sizeof(map->entries[0].skill_id) - 1);
        strncpy(map->entries[map->count].version, version,
                sizeof(map->entries[0].version) - 1);
        map->count++;
    }
}

static void version_map_save(const version_map_t *map)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        return;
    }

    for (int i = 0; i < map->count; i++) {
        cJSON_AddStringToObject(root, map->entries[i].skill_id,
                                map->entries[i].version);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        return;
    }

    FILE *f = fopen(AGENT_SKILL_SYNC_VERSION_FILE, "w");
    if (f) {
        fputs(json, f);
        fclose(f);
    }

    free(json);
}

/* ── Bitable API helpers ───────────────────────────────────────── */

/**
 * Check if target_device array contains our device type or "all".
 */
static bool device_matches(cJSON *target_device)
{
    if (!cJSON_IsArray(target_device)) {
        return true; /* No filter = matches all */
    }

    const char *my_type = AGENT_SKILL_SYNC_DEVICE_TYPE;
    cJSON *item = NULL;

    cJSON_ArrayForEach(item, target_device) {
        if (cJSON_IsString(item)) {
            if (strcmp(item->valuestring, "all") == 0 ||
                strcmp(item->valuestring, my_type) == 0) {
                return true;
            }
        }
    }

    return false;
}

/**
 * Write a skill markdown file to the local skills directory.
 */
static int write_skill_file(const char *skill_id, const char *content)
{
    char path[128];
    snprintf(path, sizeof(path), "%s%s.md", AGENT_SKILLS_DIR, skill_id);

    FILE *f = fopen(path, "w");
    if (!f) {
        syslog(LOG_ERR, "[%s] Cannot write skill: %s\n", TAG, path);
        return ERROR;
    }

    fputs(content, f);
    fclose(f);
    return OK;
}

/**
 * Remove a local skill file.
 */
static int remove_skill_file(const char *skill_id)
{
    char path[128];
    snprintf(path, sizeof(path), "%s%s.md", AGENT_SKILLS_DIR, skill_id);
    return unlink(path);
}

/**
 * Fetch one page of bitable records.
 * Returns cJSON root (caller must free) or NULL on error.
 */
static cJSON *fetch_records_page(const char *page_token)
{
    /* Ensure token is valid */
    if (feishu_token_expired()) {
        if (feishu_get_app_token() != OK) {
            syslog(LOG_ERR, "[%s] Failed to refresh Feishu token\n", TAG);
            return NULL;
        }
    }

    /* Build request path */
    char path[512];
    int off = snprintf(path, sizeof(path),
        "/open-apis/bitable/v1/apps/%s/tables/%s/records"
        "?page_size=500",
        AGENT_SKILL_SYNC_APP_TOKEN,
        AGENT_SKILL_SYNC_TABLE_ID);

    if (page_token && page_token[0]) {
        snprintf(path + off, sizeof(path) - (size_t)off,
                 "&page_token=%s", page_token);
    }

    /* Auth header */
    char auth_val[520];
    snprintf(auth_val, sizeof(auth_val), "Bearer %s", s_access_token);
    vela_header_t headers[] = {
        { "Authorization", auth_val },
        { NULL, NULL }
    };

    /* Allocate response buffer */
    char *resp = malloc(AGENT_SKILL_SYNC_RESP_SIZE);
    if (!resp) {
        syslog(LOG_ERR, "[%s] OOM for response buffer\n", TAG);
        return NULL;
    }

    size_t body_len = 0;
    int status = feishu_https_request("GET", path, headers, NULL, 0,
                                      resp, AGENT_SKILL_SYNC_RESP_SIZE,
                                      &body_len);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] Bitable API HTTP %d: %.200s\n",
               TAG, status, resp);
        free(resp);
        return NULL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);

    if (!root) {
        syslog(LOG_ERR, "[%s] Bitable API: invalid JSON response\n", TAG);
        return NULL;
    }

    /* Check API-level error code */
    cJSON *code = cJSON_GetObjectItem(root, "code");
    if (cJSON_IsNumber(code) && (int)code->valuedouble != 0) {
        cJSON *msg = cJSON_GetObjectItem(root, "msg");
        syslog(LOG_ERR, "[%s] Bitable API error: code=%d msg=%s\n",
               TAG, (int)code->valuedouble,
               (msg && cJSON_IsString(msg)) ? msg->valuestring : "?");
        cJSON_Delete(root);
        return NULL;
    }

    return root;
}

/* ── Main sync logic ───────────────────────────────────────────── */

int skill_sync_from_bitable(void)
{
    /* Validate configuration */
    const char *app_token = AGENT_SKILL_SYNC_APP_TOKEN;
    const char *table_id  = AGENT_SKILL_SYNC_TABLE_ID;

    if (!app_token[0] || !table_id[0]) {
        syslog(LOG_WARNING,
               "[%s] Skill sync disabled: app_token or table_id not set\n",
               TAG);
        return ERROR;
    }

    if (s_app_id[0] == '\0' || s_app_secret[0] == '\0') {
        syslog(LOG_WARNING,
               "[%s] Skill sync disabled: Feishu credentials not configured\n",
               TAG);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Starting skill sync (mode=%s, device=%s)\n",
           TAG,
           AGENT_SKILL_SYNC_MODE == 1 ? "full" : "incremental",
           AGENT_SKILL_SYNC_DEVICE_TYPE);

    /* Load local version map */
    version_map_t *vmap = malloc(sizeof(version_map_t));
    if (!vmap) {
        syslog(LOG_ERR, "[%s] OOM for version map\n", TAG);
        return ERROR;
    }
    memset(vmap, 0, sizeof(*vmap));

    if (AGENT_SKILL_SYNC_MODE == 0) {
        version_map_load(vmap);
    }

    /* Track which remote skill_ids we've seen (for orphan cleanup) */
    char remote_ids[128][64];
    int remote_count = 0;

    int synced = 0;
    int skipped = 0;
    int errors = 0;
    bool has_more = true;
    char page_token[256] = "";

    while (has_more) {
        cJSON *root = fetch_records_page(page_token);
        if (!root) {
            errors++;
            break;
        }

        cJSON *data = cJSON_GetObjectItem(root, "data");
        if (!data) {
            cJSON_Delete(root);
            break;
        }

        /* Pagination */
        cJSON *has_more_j = cJSON_GetObjectItem(data, "has_more");
        cJSON *page_token_j = cJSON_GetObjectItem(data, "page_token");

        has_more = cJSON_IsBool(has_more_j) && cJSON_IsTrue(has_more_j);
        if (has_more && cJSON_IsString(page_token_j)) {
            strncpy(page_token, page_token_j->valuestring,
                    sizeof(page_token) - 1);
        } else {
            has_more = false;
        }

        /* Process records */
        cJSON *items = cJSON_GetObjectItem(data, "items");
        if (!cJSON_IsArray(items)) {
            cJSON_Delete(root);
            break;
        }

        cJSON *record = NULL;
        cJSON_ArrayForEach(record, items) {
            cJSON *fields = cJSON_GetObjectItem(record, "fields");
            if (!fields) {
                continue;
            }

            /* Extract fields */
            cJSON *j_skill_id = cJSON_GetObjectItem(fields, "skill_id");
            cJSON *j_content  = cJSON_GetObjectItem(fields, "prompt_content");
            cJSON *j_version  = cJSON_GetObjectItem(fields, "version");
            cJSON *j_enabled  = cJSON_GetObjectItem(fields, "enabled");
            cJSON *j_device   = cJSON_GetObjectItem(fields, "target_device");

            if (!cJSON_IsString(j_skill_id) || !cJSON_IsString(j_content)) {
                continue;
            }

            const char *skill_id = j_skill_id->valuestring;
            const char *content  = j_content->valuestring;
            const char *version  = cJSON_IsString(j_version)
                                       ? j_version->valuestring : "0";

            /* Skip disabled */
            if (cJSON_IsBool(j_enabled) && !cJSON_IsTrue(j_enabled)) {
                continue;
            }

            /* Filter by device type */
            if (!device_matches(j_device)) {
                continue;
            }

            /* Track remote id for orphan cleanup */
            if (remote_count < 128) {
                strncpy(remote_ids[remote_count], skill_id,
                        sizeof(remote_ids[0]) - 1);
                remote_ids[remote_count][63] = '\0';
                remote_count++;
            }

            /* Incremental: skip if version unchanged */
            if (AGENT_SKILL_SYNC_MODE == 0) {
                const char *local_ver = version_map_get(vmap, skill_id);
                if (local_ver && strcmp(local_ver, version) == 0) {
                    skipped++;
                    continue;
                }
            }

            /* Write skill file */
            if (write_skill_file(skill_id, content) == OK) {
                version_map_set(vmap, skill_id, version);
                synced++;
                syslog(LOG_INFO, "[%s] Synced: %s (v%s)\n",
                       TAG, skill_id, version);
            } else {
                errors++;
            }
        }

        cJSON_Delete(root);
    }

    /* Full mode: clean orphaned local skills not in remote */
    if (AGENT_SKILL_SYNC_MODE == 1 && remote_count > 0) {
        DIR *dir = opendir(AGENT_SKILLS_DIR);
        if (dir) {
            struct dirent *ent;
            while ((ent = readdir(dir)) != NULL) {
                const char *name = ent->d_name;
                size_t nlen = strlen(name);
                if (nlen < 4 || strcmp(name + nlen - 3, ".md") != 0) {
                    continue;
                }
                if (name[0] == '.') {
                    continue;
                }

                /* Extract skill_id from filename (strip .md) */
                char sid[64];
                size_t slen = nlen - 3;
                if (slen >= sizeof(sid)) {
                    slen = sizeof(sid) - 1;
                }
                memcpy(sid, name, slen);
                sid[slen] = '\0';

                /* Check if still in remote */
                bool found = false;
                for (int i = 0; i < remote_count; i++) {
                    if (strcmp(remote_ids[i], sid) == 0) {
                        found = true;
                        break;
                    }
                }

                if (!found) {
                    remove_skill_file(sid);
                    syslog(LOG_INFO, "[%s] Removed orphan: %s\n", TAG, sid);
                }
            }
            closedir(dir);
        }
    }

    /* Save updated version map */
    version_map_save(vmap);
    free(vmap);

    syslog(LOG_INFO,
           "[%s] Skill sync complete: %d synced, %d skipped, %d errors\n",
           TAG, synced, skipped, errors);

    return errors > 0 ? ERROR : OK;
}

#endif /* AGENT_SKILL_SYNC_ENABLED */
