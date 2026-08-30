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

#include "tools/tool_shell.h"
#include "agent_config.h"
#include "agent_compat.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>

#include "cJSON.h"

static const char *TAG = "shell";

#define SHELL_OUTPUT_MAX  4096
#define SHELL_CMD_MAX     256

/* Commands that are too dangerous to run from LLM */
#if AGENT_SHELL_SECURITY == AGENT_SHELL_SECURITY_ALLOWLIST
static const char *s_blocked[] = {
    "reboot", "shutdown", "poweroff", "mkfs",
    "dd", "flash_eraseall", "format", "rm",
    "mv", "mount", "umount", "insmod", "rmmod",
    "kill", "killall", "ifconfig", "route",
    "iptables", "passwd", "su", "chmod", "chown",
    NULL
};
#endif

/* Critical commands that are blocked even in FULL mode */
#if AGENT_SHELL_SECURITY == AGENT_SHELL_SECURITY_FULL
static const char *s_critical[] = {
    "reboot", "shutdown", "poweroff", "mkfs",
    "format", "flash_eraseall", "insmod", "rmmod",
    NULL
};
#endif

/* Safe commands allowed in ALLOWLIST mode (aligned with openclaw DEFAULT_SAFE_BINS) */
#if AGENT_SHELL_SECURITY == AGENT_SHELL_SECURITY_ALLOWLIST
static const char *s_allowed[] = {
    /* Built-in implementations */
    "free", "ps", "df", "uptime", "uname", "cat", "ls",
    
    /* Basic utilities */
    "echo", "date", "env", "pwd", "id", "whoami",
    
    /* Network tools */
    "curl", "wget", "ping", "nslookup",
    
    /* Graphics/UI */
    "lvctl", "nxplayer", "fbcapture", "input", "pm", "am",
    
    /* Text processing (openclaw-compatible) */
    "jq", "cut", "uniq", "head", "tail", "tr", "wc", "grep", "sed",
    
    /* System info */
    "dmesg", "hexdump",
    
    /* Interpreters (if compiled) */
    "lua", "quickjs",
    
    /* AI Agent own NSH commands (safe to call via popen) */
    "voice_start", "voice_stop",
    "voice_test_tts", "voice_test_asr",
    "set_voice_tts", "set_voice_asr",
    "cron_start", "heap_info", "config_show",
    "node_list", "node_start", "node_stop", "net_status",
    "router_status", "router_profile", "router_set",
    "set_gateway", "set_mqtt",
    "session_list", "memory_read",
    "mcp_list", "mcp_discover",

    /* VelaGuard read-only bring-up tools (stage1 ai_agent) */
    "vgmodbus", "vgstats", "vgcfg", "vgnet",

    NULL
};
#endif

/* Shell meta-characters that could be used for injection */
#if AGENT_SHELL_SECURITY == AGENT_SHELL_SECURITY_ALLOWLIST
static bool has_shell_meta(const char *cmd)
{
    const char *meta = "|;&`$(){}><\n\\";
    for (const char *p = cmd; *p; p++) {
        if (strchr(meta, *p)) return true;
    }
    return false;
}
#endif

static int is_blocked(const char *cmd)
{
#if AGENT_SHELL_SECURITY == AGENT_SHELL_SECURITY_DENY
    /* Deny mode: block everything */
    return 1;
#elif AGENT_SHELL_SECURITY == AGENT_SHELL_SECURITY_FULL
    /* Full mode: allow most commands, only block critical ones.
     * This mode is designed for skill authoring and development.
     * Skills can use pipes, redirects, and command chaining. */
    
    /* Extract command name */
    char first[64];
    int i = 0;
    while (cmd[i] == ' ') i++;
    int j = 0;
    while (cmd[i] && cmd[i] != ' ' && j < (int)sizeof(first) - 1)
        first[j++] = cmd[i++];
    first[j] = '\0';

    /* Strip path prefix: /bin/rm -> rm */
    const char *basename = strrchr(first, '/');
    const char *name = basename ? basename + 1 : first;

    /* Block critical commands only */
    for (int k = 0; s_critical[k]; k++) {
        if (strcmp(name, s_critical[k]) == 0)
            return 1;
    }
    
    return 0;  /* allow */
#else
    /* Allowlist mode (default): strict whitelist + no shell meta-characters.
     * This is the production-safe mode. */
    
    /* Reject any shell meta-characters (pipe, redirect, subshell, etc.) */
    if (has_shell_meta(cmd))
        return 1;

    /* Extract the first token (command name) */
    char first[64];
    int i = 0;
    while (cmd[i] == ' ') i++;
    int j = 0;
    while (cmd[i] && cmd[i] != ' ' && j < (int)sizeof(first) - 1)
        first[j++] = cmd[i++];
    first[j] = '\0';

    /* Strip path prefix: /bin/rm -> rm, /usr/bin/dd -> dd */
    const char *basename = strrchr(first, '/');
    const char *name = basename ? basename + 1 : first;

    /* Check whitelist first */
    for (int k = 0; s_allowed[k]; k++) {
        if (strcmp(name, s_allowed[k]) == 0)
            return 0;  /* allowed */
    }

    /* Then check blacklist */
    for (int k = 0; s_blocked[k]; k++) {
        if (strcmp(name, s_blocked[k]) == 0)
            return 1;  /* blocked */
    }
    
    /* Default deny in allowlist mode */
    return 1;
#endif
}

/* Read a /proc file into buffer, return bytes read */
static size_t read_proc_file(const char *path, char *buf, size_t buf_size)
{
    FILE *fp = fopen(path, "r");
    if (!fp) return 0;
    size_t n = fread(buf, 1, buf_size - 1, fp);
    buf[n] = '\0';
    fclose(fp);
    return n;
}

/* Built-in "free" — read /proc/meminfo */
static int builtin_free(char *output, size_t output_size)
{
    char buf[1024] = {0};
    if (read_proc_file("/proc/meminfo", buf, sizeof(buf)) == 0)
        return ERROR;
    snprintf(output, output_size, "%s", buf);
    return OK;
}

/* Built-in "ps" — read /proc/<pid>/status for each pid */
static int builtin_ps(char *output, size_t output_size)
{
    DIR *d = opendir("/proc");
    if (!d) return ERROR;

    size_t off = 0;
    off += snprintf(output + off, output_size - off,
                    "%-6s %-4s %-8s %-20s %s\n",
                    "PID", "PRI", "STACK", "STATUS", "NAME");

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && off < output_size - 128) {
        /* Only numeric dirs are PIDs */
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9')
            continue;

        char path[128];
        char buf[512] = {0};

        snprintf(path, sizeof(path), "/proc/%s/status", ent->d_name);
        if (read_proc_file(path, buf, sizeof(buf)) > 0) {
            /* Trim trailing newline */
            char *nl = strchr(buf, '\n');
            if (nl) *nl = '\0';
            off += snprintf(output + off, output_size - off,
                            "%-6s %s\n", ent->d_name, buf);
        }
    }
    closedir(d);
    return OK;
}

/* Built-in "df" — statfs on common mount points */
static int builtin_df(char *output, size_t output_size)
{
    static const char *mounts[] = { "/", "/tmp", "/data", NULL };
    size_t off = 0;
    off += snprintf(output + off, output_size - off,
                    "%-12s %12s %12s %12s\n",
                    "Filesystem", "Total", "Used", "Free");

    for (int i = 0; mounts[i]; i++) {
        struct statfs sfs;
        if (statfs(mounts[i], &sfs) == 0) {
            unsigned long total = (unsigned long)sfs.f_blocks * sfs.f_bsize;
            unsigned long avail = (unsigned long)sfs.f_bavail * sfs.f_bsize;
            unsigned long used  = total - avail;
            off += snprintf(output + off, output_size - off,
                            "%-12s %10luKB %10luKB %10luKB\n",
                            mounts[i], total / 1024, used / 1024, avail / 1024);
        }
        if (off >= output_size - 1) break;
    }
    return OK;
}

/* Built-in "uptime" — read /proc/uptime */
static int builtin_uptime(char *output, size_t output_size)
{
    char buf[128] = {0};
    if (read_proc_file("/proc/uptime", buf, sizeof(buf)) == 0)
        return ERROR;
    snprintf(output, output_size, "Uptime: %s", buf);
    return OK;
}

/* Built-in "uname" — read /proc/version */
static int builtin_uname(char *output, size_t output_size)
{
    char buf[256] = {0};
    if (read_proc_file("/proc/version", buf, sizeof(buf)) > 0) {
        snprintf(output, output_size, "%s", buf);
        return OK;
    }
    /* Fallback: identify as NuttX */
    snprintf(output, output_size, "NuttX (version info unavailable)");
    return OK;
}

/* Allowed path prefixes for file access (symlink escape protection) */
static const char *s_allowed_prefixes[] = {
    "/proc/", "/data/agent/", "/tmp/", NULL
};

static bool is_path_allowed(const char *path)
{
    char resolved[PATH_MAX];
    /* Resolve symlinks to get the real path */
    if (realpath(path, resolved) == NULL) {
        /* If file doesn't exist, check the raw path */
        strncpy(resolved, path, sizeof(resolved) - 1);
        resolved[sizeof(resolved) - 1] = '\0';
    }

    /* Normalise: strip trailing slash for comparison (except root "/") */
    size_t rlen = strlen(resolved);
    if (rlen > 1 && resolved[rlen - 1] == '/')
        resolved[--rlen] = '\0';

    for (int i = 0; s_allowed_prefixes[i]; i++) {
        /* Compare against prefix with and without its trailing slash */
        size_t plen = strlen(s_allowed_prefixes[i]);
        /* Strip trailing slash from prefix for comparison */
        size_t cmp_len = (plen > 1 && s_allowed_prefixes[i][plen - 1] == '/')
            ? plen - 1 : plen;
        /* Allow exact match (the directory itself) or path inside it */
        if (strncmp(resolved, s_allowed_prefixes[i], cmp_len) == 0
            && (resolved[cmp_len] == '\0' || resolved[cmp_len] == '/'))
            return true;
    }
    return false;
}

/* Built-in "cat" — read any readable file (with path restriction) */
static int builtin_cat(const char *path, char *output, size_t output_size)
{
    if (!path || !path[0]) return ERROR;
    if (!is_path_allowed(path)) {
        snprintf(output, output_size, "Access denied: path outside allowed directories");
        return ERROR;
    }
    size_t n = read_proc_file(path, output, output_size);
    return n > 0 ? OK : ERROR;
}

/* Built-in "dmesg" — read kernel log via /dev/kmsg */
static int builtin_dmesg(char *output, size_t output_size)
{
    /* Try /dev/kmsg (NuttX kernel log device) */
    size_t n = read_proc_file("/dev/kmsg", output, output_size);
    if (n > 0)
        return OK;

    /* Fallback: read syslog buffer via /proc/kmsg */
    n = read_proc_file("/proc/kmsg", output, output_size);
    if (n > 0)
        return OK;

    snprintf(output, output_size, "(dmesg not available on this platform)");
    return OK;
}

/* Built-in command via popen — execute a whitelisted command and capture output */
static int builtin_popen_cmd(const char *cmd, char *output, size_t output_size)
{
#ifdef CONFIG_SYSTEM_POPEN
    FILE *fp = popen(cmd, "r");
    if (!fp) {
        snprintf(output, output_size,
                 "Failed to execute: %s (%s)", cmd, strerror(errno));
        return ERROR;
    }

    size_t n = fread(output, 1, output_size - 1, fp);
    output[n] = '\0';
    int status = pclose(fp);
    if (status != 0 && n == 0) {
        snprintf(output, output_size,
                 "Command failed with exit code %d", WEXITSTATUS(status));
        return ERROR;
    }

    return (status == 0) ? OK : ERROR;
#else
    (void)cmd;
    snprintf(output, output_size, "popen not available (enable CONFIG_SYSTEM_POPEN)");
    return ERROR;
#endif
}

/* Built-in "ls" — list directory (with path restriction) */
static int builtin_ls(const char *path, char *output, size_t output_size)
{
    /* Strip trailing slash for display, but keep for opendir */
    char dir_buf[PATH_MAX];
    const char *dir = (path && path[0]) ? path : "/data/agent";

    /* Normalise: remove trailing slash (except root "/") so path check works */
    strncpy(dir_buf, dir, sizeof(dir_buf) - 1);
    dir_buf[sizeof(dir_buf) - 1] = '\0';
    size_t dlen = strlen(dir_buf);
    if (dlen > 1 && dir_buf[dlen - 1] == '/')
        dir_buf[dlen - 1] = '\0';

    if (!is_path_allowed(dir_buf) && !is_path_allowed(dir)) {
        snprintf(output, output_size, "Access denied: path outside allowed directories");
        return ERROR;
    }

    DIR *d = opendir(dir_buf);
    if (!d) {
        snprintf(output, output_size, "Cannot open directory %s: %s",
                 dir_buf, strerror(errno));
        return ERROR;
    }

    size_t off = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && off < output_size - 64) {
        off += snprintf(output + off, output_size - off, "%s\n", ent->d_name);
    }
    closedir(d);
    if (off == 0)
        snprintf(output, output_size, "(empty)");
    return OK;
}

int tool_run_shell_execute(const char *input_json, char *output, size_t output_size)
{
    const char *command = NULL;

    cJSON *root = cJSON_Parse(input_json);
    if (root) {
        cJSON *c = cJSON_GetObjectItem(root, "command");
        if (c && cJSON_IsString(c))
            command = c->valuestring;
    }

    if (!command || command[0] == '\0') {
        snprintf(output, output_size, "{\"error\":\"Missing 'command' parameter\"}");
        cJSON_Delete(root);
        return ERROR;
    }

    if (strlen(command) > SHELL_CMD_MAX) {
        snprintf(output, output_size, "{\"error\":\"Command too long (max %d)\"}", SHELL_CMD_MAX);
        cJSON_Delete(root);
        return ERROR;
    }

    if (is_blocked(command)) {
        syslog(LOG_WARNING, "[%s] Blocked dangerous command: %s\n", TAG, command);
        snprintf(output, output_size, "{\"error\":\"Command '%s' is blocked for safety\"}", command);
        cJSON_Delete(root);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Executing: %s\n", TAG, command);

    /* Heap-allocate output buffer to avoid stack overflow.
     * tool_run_shell_execute is called from parallel tool threads
     * (16KB stack) and SHELL_OUTPUT_MAX (4096) on stack would consume
     * too much of the available stack space. */
    char *cmd_buf = calloc(1, SHELL_OUTPUT_MAX);
    if (!cmd_buf) {
        snprintf(output, output_size, "{\"error\":\"OOM\"}");
        cJSON_Delete(root);
        return ERROR;
    }
    int ret = ERROR;

    /* Skip leading spaces */
    while (*command == ' ') command++;

    if (strcmp(command, "free") == 0 || strncmp(command, "free ", 5) == 0) {
        ret = builtin_free(cmd_buf, SHELL_OUTPUT_MAX);
    } else if (strcmp(command, "ps") == 0 || strncmp(command, "ps ", 3) == 0) {
        ret = builtin_ps(cmd_buf, SHELL_OUTPUT_MAX);
    } else if (strcmp(command, "df") == 0 || strncmp(command, "df ", 3) == 0) {
        ret = builtin_df(cmd_buf, SHELL_OUTPUT_MAX);
    } else if (strcmp(command, "uptime") == 0) {
        ret = builtin_uptime(cmd_buf, SHELL_OUTPUT_MAX);
    } else if (strcmp(command, "uname") == 0 || strncmp(command, "uname ", 6) == 0) {
        ret = builtin_uname(cmd_buf, SHELL_OUTPUT_MAX);
    } else if (strncmp(command, "cat ", 4) == 0) {
        const char *arg = command + 4;
        while (*arg == ' ') arg++;
        ret = builtin_cat(arg, cmd_buf, SHELL_OUTPUT_MAX);
    } else if (strncmp(command, "ls", 2) == 0) {
        const char *arg = command + 2;
        while (*arg == ' ') arg++;
        ret = builtin_ls(arg[0] ? arg : NULL, cmd_buf, SHELL_OUTPUT_MAX);
    } else if (strcmp(command, "dmesg") == 0 || strncmp(command, "dmesg ", 6) == 0) {
        ret = builtin_dmesg(cmd_buf, SHELL_OUTPUT_MAX);
    } else if (strncmp(command, "pm ", 3) == 0 || strcmp(command, "pm") == 0) {
        ret = builtin_popen_cmd(command, cmd_buf, SHELL_OUTPUT_MAX);
    } else if (strncmp(command, "am ", 3) == 0 || strcmp(command, "am") == 0) {
        ret = builtin_popen_cmd(command, cmd_buf, SHELL_OUTPUT_MAX);
    } else {
#ifdef CONFIG_SYSTEM_POPEN
        /* Execute via popen() - available when CONFIG_SYSTEM_POPEN is enabled.
         * Command has already passed security checks (is_blocked). */
        FILE *fp = popen(command, "r");
        if (fp) {
            size_t n = fread(cmd_buf, 1, SHELL_OUTPUT_MAX - 1, fp);
            cmd_buf[n] = '\0';
            int status = pclose(fp);
            ret = (status == 0) ? OK : ERROR;
            if (ret != OK && n == 0) {
                snprintf(cmd_buf, SHELL_OUTPUT_MAX,
                         "Command failed with exit code %d", WEXITSTATUS(status));
            }
            syslog(LOG_INFO, "[%s] popen(%s) exit=%d, %zu bytes\n",
                   TAG, command, status, n);
        } else {
            syslog(LOG_ERR, "[%s] popen(%s) failed: %s\n",
                   TAG, command, strerror(errno));
            snprintf(cmd_buf, SHELL_OUTPUT_MAX,
                     "Failed to execute: %s (%s)", command, strerror(errno));
            ret = ERROR;
        }
#else
        /* popen() not available - only builtin commands work */
        syslog(LOG_WARNING, "[%s] Unsupported command: %s\n", TAG, command);
        snprintf(cmd_buf, SHELL_OUTPUT_MAX,
                 "Unsupported command: %s (enable CONFIG_SYSTEM_POPEN for external commands)",
                 command);
        ret = ERROR;
#endif
    }

    cJSON_Delete(root);

    /* Always return structured JSON so the LLM sees stdout even on non-zero exit. */
    {
        int exit_code = (ret == OK) ? 0 : 1;
        cJSON *result = cJSON_CreateObject();

        cJSON_AddNumberToObject(result, "exit_code", exit_code);
        cJSON_AddStringToObject(result, "output", cmd_buf);
        if (exit_code != 0) {
            cJSON_AddStringToObject(result, "error", "non-zero exit");
        }

        char *json_str = cJSON_PrintUnformatted(result);
        cJSON_Delete(result);
        free(cmd_buf);

        if (!json_str) {
            snprintf(output, output_size, "{\"exit_code\":1,\"output\":\"\"}");
            return ERROR;
        }

        strncpy(output, json_str, output_size - 1);
        output[output_size - 1] = '\0';
        free(json_str);
        syslog(LOG_INFO, "[%s] Result: %d bytes (exit=%d)\n",
            TAG, (int)strlen(output), exit_code);
        return OK;
    }
}
