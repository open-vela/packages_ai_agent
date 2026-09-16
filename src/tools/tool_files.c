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

#include "tools/tool_files.h"
#include "agent_config.h"
#include "agent_compat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include "cJSON.h"

static const char *TAG = "tool_files";

#define MAX_FILE_SIZE (32 * 1024)

/* VelaGuard reports/alarms live outside AGENT_DATA_DIR but on the same /data mount. */
#define VG_DATA_ROOT "/data/velaguard"

static bool path_under_resolved_root(const char *resolved, const char *logical_root)
{
    char root[PATH_MAX];
    const char *base = logical_root;

    if (realpath(logical_root, root) != NULL) {
        base = root;
    }

    size_t len = strlen(base);
    return strncmp(resolved, base, len) == 0
        && (resolved[len] == '/' || resolved[len] == '\0');
}

static bool logical_path_allowed(const char *path)
{
    size_t agent_len = strlen(AGENT_DATA_DIR);

    if (strncmp(path, AGENT_DATA_DIR, agent_len) == 0
        && (path[agent_len] == '/' || path[agent_len] == '\0')) {
        return true;
    }

    if (strncmp(path, VG_DATA_ROOT, sizeof(VG_DATA_ROOT) - 1) == 0
        && (path[sizeof(VG_DATA_ROOT) - 1] == '/'
            || path[sizeof(VG_DATA_ROOT) - 1] == '\0')) {
        return true;
    }

    return false;
}

static bool path_has_prefix(const char *path, const char *prefix)
{
    if (!prefix || prefix[0] == '\0') {
        return true;
    }

    size_t plen = strlen(prefix);
    while (plen > 0 && prefix[plen - 1] == '/') {
        plen--;
    }

    if (strlen(path) < plen) {
        return false;
    }

    if (strncmp(path, prefix, plen) != 0) {
        return false;
    }

    return path[plen] == '\0' || path[plen] == '/';
}

/**
 * Validate that path resolves to a location under AGENT_DATA_DIR.
 * Uses realpath() to resolve symlinks, preventing symlink escape attacks.
 * Falls back to raw path check if the file does not yet exist.
 */
static bool validate_path(const char *path)
{
    if (!path) {
        return false;
    }

    /* Reject explicit traversal components */
    if (strstr(path, "..") != NULL) {
        return false;
    }

    if (!logical_path_allowed(path)) {
        return false;
    }

    /* Resolve symlinks (/data -> /mnt/emmc/data) and verify physical path. */
    char resolved[PATH_MAX];

    if (realpath(path, resolved) != NULL) {
        if (!path_under_resolved_root(resolved, AGENT_DATA_DIR)
            && !path_under_resolved_root(resolved, VG_DATA_ROOT)) {
            syslog(LOG_WARNING,
                   "[%s] Path escaped allowed data dirs via symlink: %s -> %s\n",
                   TAG, path, resolved);
            return false;
        }
    }

    return true;
}

/* Files that the LLM must not overwrite via write_file / edit_file.
 * These can only be modified through CLI or dedicated config commands. */
static const char *s_protected_files[] = {
    AGENT_CONFIG_FILE,   /* /data/agent/config/config.json */
    AGENT_SOUL_FILE,     /* /data/agent/config/SOUL.md     */
    AGENT_USER_FILE,     /* /data/agent/config/USER.md     */
    NULL
};

static bool is_write_protected(const char *path)
{
    char resolved[PATH_MAX];
    const char *check = path;

    if (realpath(path, resolved) != NULL) {
        check = resolved;
    }

    for (int i = 0; s_protected_files[i]; i++) {
        if (strcmp(check, s_protected_files[i]) == 0) {
            return true;
        }
    }

    return false;
}
/**
 * Ensure all parent directories of `path` exist.
 * Only creates directories under AGENT_DATA_DIR.
 */
static void ensure_parent_dirs(const char *path)
{
    char tmp[512];
    size_t start;

    snprintf(tmp, sizeof(tmp), "%s", path);

    if (strncmp(path, AGENT_DATA_DIR, strlen(AGENT_DATA_DIR)) == 0) {
        start = strlen(AGENT_DATA_DIR);
    } else if (strncmp(path, VG_DATA_ROOT, sizeof(VG_DATA_ROOT) - 1) == 0) {
        start = sizeof(VG_DATA_ROOT) - 1;
    } else {
        return;
    }

    for (char *p = tmp + start; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
}

/* ── read_file ─────────────────────────────────────────────── */

int tool_read_file_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    const char *path = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    if (!validate_path(path)) {
        snprintf(output, output_size,
                 "Error: path must be under %s/ or %s/ and must not contain '..'",
                 AGENT_DATA_DIR, VG_DATA_ROOT);
        cJSON_Delete(root);
        return ERROR;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(output, output_size, "Error: file not found: %s", path);
        cJSON_Delete(root);
        return ERROR;
    }

    size_t max_read = output_size - 1;
    if (max_read > MAX_FILE_SIZE) max_read = MAX_FILE_SIZE;

    size_t n = fread(output, 1, max_read, f);
    output[n] = '\0';
    fclose(f);

    syslog(LOG_INFO, "[%s] read_file: %s (%d bytes)\n", TAG, path, (int)n);
    cJSON_Delete(root);
    return OK;
}

/* ── write_file ────────────────────────────────────────────── */

int tool_write_file_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    const char *path    = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    const char *content = cJSON_GetStringValue(cJSON_GetObjectItem(root, "content"));

    if (!validate_path(path)) {
        snprintf(output, output_size,
                 "Error: path must be under %s/ or %s/ and must not contain '..'",
                 AGENT_DATA_DIR, VG_DATA_ROOT);
        cJSON_Delete(root);
        return ERROR;
    }
    if (is_write_protected(path)) {
        syslog(LOG_WARNING, "[%s] Blocked write to protected file: %s\n", TAG, path);
        snprintf(output, output_size,
                 "Error: '%s' is a protected system file and cannot be modified", path);
        cJSON_Delete(root);
        return ERROR;
    }
    if (!content) {
        snprintf(output, output_size, "Error: missing 'content' field");
        cJSON_Delete(root);
        return ERROR;
    }

    ensure_parent_dirs(path);

    FILE *f = fopen(path, "w");
    if (!f) {
        snprintf(output, output_size, "Error: cannot open file for writing: %s (errno=%d)", path, errno);
        cJSON_Delete(root);
        return ERROR;
    }

    size_t len     = strlen(content);
    size_t written = fwrite(content, 1, len, f);
    fclose(f);

    if (written != len) {
        snprintf(output, output_size,
                 "Error: wrote %d of %d bytes to %s", (int)written, (int)len, path);
        cJSON_Delete(root);
        return ERROR;
    }

    snprintf(output, output_size, "OK: wrote %d bytes to %s", (int)written, path);
    syslog(LOG_INFO, "[%s] write_file: %s (%d bytes)\n", TAG, path, (int)written);
    cJSON_Delete(root);
    return OK;
}

/* ── edit_file ─────────────────────────────────────────────── */

int tool_edit_file_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    const char *path    = cJSON_GetStringValue(cJSON_GetObjectItem(root, "path"));
    const char *old_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "old_string"));
    const char *new_str = cJSON_GetStringValue(cJSON_GetObjectItem(root, "new_string"));

    if (!validate_path(path)) {
        snprintf(output, output_size,
                 "Error: path must be under %s/ or %s/ and must not contain '..'",
                 AGENT_DATA_DIR, VG_DATA_ROOT);
        cJSON_Delete(root);
        return ERROR;
    }
    if (is_write_protected(path)) {
        syslog(LOG_WARNING, "[%s] Blocked edit of protected file: %s\n", TAG, path);
        snprintf(output, output_size,
                 "Error: '%s' is a protected system file and cannot be modified", path);
        cJSON_Delete(root);
        return ERROR;
    }
    if (!old_str || !new_str) {
        snprintf(output, output_size, "Error: missing 'old_string' or 'new_string' field");
        cJSON_Delete(root);
        return ERROR;
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(output, output_size, "Error: file not found: %s", path);
        cJSON_Delete(root);
        return ERROR;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > MAX_FILE_SIZE) {
        snprintf(output, output_size, "Error: file too large or empty (%ld bytes)", file_size);
        fclose(f);
        cJSON_Delete(root);
        return ERROR;
    }

    size_t old_len = strlen(old_str);
    size_t new_len = strlen(new_str);
    size_t max_result = (size_t)file_size + (new_len > old_len ? new_len - old_len : 0) + 1;

    char *buf    = malloc((size_t)file_size + 1);
    char *result = malloc(max_result);
    if (!buf || !result) {
        free(buf); free(result); fclose(f);
        snprintf(output, output_size, "Error: out of memory");
        cJSON_Delete(root);
        return ERROR;
    }

    size_t n = fread(buf, 1, (size_t)file_size, f);
    buf[n] = '\0';
    fclose(f);

    char *pos = strstr(buf, old_str);
    if (!pos) {
        snprintf(output, output_size, "Error: old_string not found in %s", path);
        free(buf); free(result);
        cJSON_Delete(root);
        return ERROR;
    }

    size_t prefix_len  = (size_t)(pos - buf);
    size_t suffix_off  = prefix_len + old_len;
    size_t suffix_len  = n - suffix_off;

    memcpy(result,                        buf,           prefix_len);
    memcpy(result + prefix_len,           new_str,       new_len);
    memcpy(result + prefix_len + new_len, buf + suffix_off, suffix_len);
    size_t total = prefix_len + new_len + suffix_len;
    result[total] = '\0';
    free(buf);

    f = fopen(path, "w");
    if (!f) {
        snprintf(output, output_size, "Error: cannot open file for writing: %s", path);
        free(result); cJSON_Delete(root);
        return ERROR;
    }
    fwrite(result, 1, total, f);
    fclose(f);
    free(result);

    snprintf(output, output_size,
             "OK: edited %s (replaced %d bytes with %d bytes)", path, (int)old_len, (int)new_len);
    syslog(LOG_INFO, "[%s] edit_file: %s\n", TAG, path);
    cJSON_Delete(root);
    return OK;
}

/* ── list_dir (recursive helper) ────────────────────────────── */

static size_t list_dir_recursive(const char *dir_path, const char *prefix,
                                  char *output, size_t output_size, size_t off,
                                  int *count, int depth)
{
    if (depth > 4) return off;  /* prevent infinite recursion */

    DIR *dir = opendir(dir_path);
    if (!dir) return off;

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && off < output_size - 1) {
        /* Skip . and .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        char full_path[512];
        snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, ent->d_name);

        if (prefix && !path_has_prefix(full_path, prefix)) continue;

        struct stat st;
        if (stat(full_path, &st) == 0 && S_ISDIR(st.st_mode)) {
            off += snprintf(output + off, output_size - off, "%s/\n", full_path);
            (*count)++;
            off = list_dir_recursive(full_path, prefix, output, output_size, off, count, depth + 1);
        } else {
            off += snprintf(output + off, output_size - off, "%s\n", full_path);
            (*count)++;
        }
    }
    closedir(dir);
    return off;
}

int tool_list_dir_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    const char *prefix = NULL;
    if (root) {
        cJSON *pfx = cJSON_GetObjectItem(root, "prefix");
        if (pfx && cJSON_IsString(pfx)) prefix = pfx->valuestring;
    }

    int count = 0;
    size_t off = list_dir_recursive(AGENT_DATA_DIR, prefix, output, output_size, 0, &count, 0);
    (void)off;

    if (count == 0) snprintf(output, output_size, "(no files found)");

    syslog(LOG_INFO, "[%s] list_dir: %d entries (prefix=%s)\n", TAG, count, prefix ? prefix : "(none)");
    cJSON_Delete(root);
    return OK;
}
