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

/* 解析失败日志的限流状态。
 *
 * 背景: 真机上 config.json 被手工 echo 写坏后, 每次 claw_config_get() 都
 * 走一遍 load_json(), 于是同一句 "is not valid JSON (367 bytes)" 刷了几十
 * 行 —— 配置被读的次数远多于它被写的次数 (llm_proxy / network_manager /
 * tool_guard / ... 各自 init 时都要读若干 key)。
 *
 * 规则: 同一份坏文件只报一次; 内容变了 (长度不同) 或成功解析过一次之后再坏,
 * 都重新报一次。这样"坏了"这件事一定看得见, 但不会刷屏。
 * 状态只被 s_lock 保护下的路径访问, 不需要额外加锁。 */
static long s_bad_json_last_size = -1;
static int  s_bad_json_suppressed = 0;

static void report_bad_json(long sz)
{
    if (s_bad_json_last_size == sz) {
        /* 同一份坏文件: 只累加计数, 不再打日志 */
        s_bad_json_suppressed++;
        return;
    }

    if (s_bad_json_suppressed > 0) {
        syslog(LOG_ERR,
               "[%s] %s is not valid JSON (%ld bytes); %d further identical "
               "read(s) were suppressed\n",
               TAG, AGENT_CONFIG_FILE, sz, s_bad_json_suppressed);
    } else {
        syslog(LOG_ERR, "[%s] %s is not valid JSON (%ld bytes)\n",
               TAG, AGENT_CONFIG_FILE, sz);
    }

    s_bad_json_last_size = sz;
    s_bad_json_suppressed = 0;
}

/* bad_json: 非 NULL 时, 由本函数写入失败原因 —— true 表示"文件内容不是
 * 合法 JSON", false 表示"读不出来/内存不足"。调用方要区分这两种失败才能
 * 安全地决定能不能把文件挪走: OOM 时文件是好的, 挪走就是丢数据。 */
static cJSON *load_json_ex(bool *bad_json)
{
    FILE *f = fopen(AGENT_CONFIG_FILE, "r");
    if (bad_json) *bad_json = false;
    if (!f) return cJSON_CreateObject();

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) { fclose(f); return cJSON_CreateObject(); }

    char *buf = (char *)malloc((size_t)(sz + 1));
    if (!buf) {
        fclose(f);
        /* OOM is *not* an empty config: returning an object here would make
         * the next claw_config_set()/config_del() rewrite the file from just
         * that key and silently drop everything else. */
        syslog(LOG_ERR, "[%s] out of memory reading %s (%ld bytes)\n",
               TAG, AGENT_CONFIG_FILE, (long)sz);
        return NULL;
    }

    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root) {
        /* 解析成功: 复位限流状态, 下次再坏时重新报一次 */
        s_bad_json_last_size = -1;
        s_bad_json_suppressed = 0;
        return root;
    }

    /* A parse failure used to return an empty object, and the next
     * claw_config_set() then merged into it and saved - wiping every other
     * key in the file. Fail the load instead; the callers below refuse to
     * write when they get NULL, so a corrupt file stays as it is.
     *
     * 关键: 这里**只读不写** —— 坏文件原样留在盘上, 既不会被空对象覆盖,
     * 也不会被"修好"(那样会丢掉用户还能手工救回来的内容)。日志走限流,
     * 见 report_bad_json()。 */
    report_bad_json((long)sz);
    if (bad_json) *bad_json = true;
    return NULL;
}

/* ── 最后一份好配置(.good) ────────────────────────────────────
 *
 * 背景(docs_ble/24 号文 §24.8): config.json 曾多次被"写成只剩 net.dns_*
 * 两项" —— 一次坏写 / 一次手工 echo 写坏之后, 网络组件启动时用
 * load_json_for_write() 把坏文件隔离成 .bad 并从空对象重建, 于是所有
 * **其它**键(LLM 后端、API key)静默消失, 云端对话退化成端侧兜底。
 *
 * 对策: 每次保存成功后留一份 .good; 读/写路径发现主文件坏掉时, 先用
 * .good 把键恢复回来, 再继续合并写。这样坏写最多丢"最后一次修改",
 * 不会再出现"整套配置一次性消失"。 */
#define AGENT_CONFIG_GOOD AGENT_CONFIG_FILE ".good"

static cJSON *load_good_copy(void)
{
    FILE *f = fopen(AGENT_CONFIG_GOOD, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (root) {
        syslog(LOG_WARNING, "[%s] primary config unusable; restored keys "
               "from last-known-good %s\n", TAG, AGENT_CONFIG_GOOD);
    }
    return root;
}

static cJSON *load_json(void)
{
    cJSON *root = load_json_ex(NULL);
    if (root == NULL) {
        root = load_good_copy();   /* 只读恢复: 主文件原样留着 */
    }
    return root;
}

#define AGENT_CONFIG_TMP AGENT_CONFIG_FILE ".tmp"

/* 写一个小文件(截断写, 0600)。用于 .good 备份。 */
static int write_blob(const char *path, const char *data)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return ERROR;

    FILE *f = fdopen(fd, "w");
    if (!f) { close(fd); return ERROR; }

    int rc = OK;
    if (fputs(data, f) == EOF || fflush(f) != 0) {
        rc = ERROR;
    }
    if (fclose(f) != 0) {
        rc = ERROR;
    }
    return rc;
}

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

    /* 留一份最后已知良好的配置。写失败不影响本次保存的结果(主文件已经
     * 成功落盘), 只是下一次坏写时少一层保险, 所以只警告不报错。 */
    {
        char *good = cJSON_PrintUnformatted(root);
        if (good) {
            if (write_blob(AGENT_CONFIG_GOOD, good) != OK) {
                syslog(LOG_WARNING, "[%s] could not refresh %s\n",
                       TAG, AGENT_CONFIG_GOOD);
            }
            free(good);
        }
    }
    return OK;
}

/* load_json() for the **write** paths.
 *
 * A corrupt config.json used to be a dead end: load_json() returns NULL, so
 * claw_config_set() / config_del() refuse to write (that part is right — a
 * partial rewrite of a *valid* file would drop every other key). But the
 * file being unparseable also meant no key could ever be set again: the user
 * had to find a serial console and run config_reset before set_llm would
 * stick, which is exactly the situation where the console is already taken
 * over by the agent CLI.
 *
 * So: on the write path, quarantine the corrupt file (rename, never delete —
 * the bytes are still there for post-mortem) and start from a clean object.
 * A valid file is never touched by this: it takes the normal load_json()
 * path and keeps the read-modify-write guarantee.
 *
 * Returns NULL only on OOM / rename failure; callers then refuse to write. */
#define AGENT_CONFIG_BAD AGENT_CONFIG_FILE ".bad"

static cJSON *load_json_for_write(void)
{
    bool bad_json = false;
    cJSON *root = load_json_ex(&bad_json);

    if (root != NULL) {
        return root;
    }

    if (!bad_json) {
        /* OOM 或读不出来: 文件本身可能是好的, 绝不能挪走 —— 挪走就是
         * 丢掉用户仅剩的配置。让写操作失败, 保持原文件原样。 */
        return NULL;
    }

    if (rename(AGENT_CONFIG_FILE, AGENT_CONFIG_BAD) != 0) {
        syslog(LOG_ERR,
               "[%s] %s is corrupt and could not be quarantined to %s: %d; "
               "refusing to write\n",
               TAG, AGENT_CONFIG_FILE, AGENT_CONFIG_BAD, errno);
        return NULL;
    }

    syslog(LOG_WARNING,
           "[%s] %s was corrupt; moved aside to %s and starting a fresh "
           "config (previous bytes preserved for inspection)\n",
           TAG, AGENT_CONFIG_FILE, AGENT_CONFIG_BAD);

    /* The corrupt file is gone from the config path: reset the rate-limit
     * state so a future corruption is reported immediately. */
    s_bad_json_last_size = -1;
    s_bad_json_suppressed = 0;

    /* ★ 不要从空对象重建 —— 那正是 "配置只剩 DNS 两项" 的形成路径:
     * 坏文件 → 空对象 + 本次调用者的键 → 其它键全丢。先用 .good 把
     * 上一份好配置找回来, 再让调用者把这次的修改合并进去。 */
    {
        cJSON *good = load_good_copy();
        if (good != NULL) {
            syslog(LOG_WARNING,
                   "[%s] rebuilt config from %s (keys preserved)\n",
                   TAG, AGENT_CONFIG_GOOD);
            return good;
        }
    }
    return cJSON_CreateObject();
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
    cJSON *root = load_json_for_write();
    if (!root) {
        pthread_mutex_unlock(&s_lock);
        return ERROR;
    }
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
    cJSON *root = load_json_for_write();
    if (!root) {
        pthread_mutex_unlock(&s_lock);
        return ERROR;
    }
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
    /* 用户显式清空时, 备份也要一起清掉, 否则下次坏写会把旧配置"复活"。 */
    unlink(AGENT_CONFIG_GOOD);
    unlink(AGENT_CONFIG_BAD);
    pthread_mutex_unlock(&s_lock);
    return ret;
}
