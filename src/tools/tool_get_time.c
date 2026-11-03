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

#include "tools/tool_get_time.h"
#include "agent_config.h"
#include "agent_compat.h"
#include "infra/http_proxy.h"
#include "infra/vela_tls.h"

#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>

static const char *TAG = "tool_time";

static const char *MONTHS[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

/* Parse "Sat, 01 Feb 2025 10:25:00 GMT" → set system clock, return formatted string */
/* v106：本地时间人话格式化（RTC 里存的就是本地时间，不再二次 +8h） */
static void format_time_cn(time_t local_epoch, char* out, size_t cap)
{
    static const char* wd[] = { "周日", "周一", "周二", "周三",
                                "周四", "周五", "周六" };
    struct tm tm_local;
    gmtime_r(&local_epoch, &tm_local);
    snprintf(out, cap, "现在是 %d月%d日 %02d:%02d（%s）",
             tm_local.tm_mon + 1, tm_local.tm_mday,
             tm_local.tm_hour, tm_local.tm_min, wd[tm_local.tm_wday]);
}

static bool parse_and_set_time(const char *date_str, char *out, size_t out_size)
{
    int day, year, hour, min, sec;
    char mon_str[4] = {0};

    if (sscanf(date_str, "%*[^,], %d %3s %d %d:%d:%d",
               &day, mon_str, &year, &hour, &min, &sec) != 6) {
        return false;
    }

    int mon = -1;
    for (int i = 0; i < 12; i++) {
        if (strcmp(mon_str, MONTHS[i]) == 0) { mon = i; break; }
    }
    if (mon < 0) return false;

    struct tm tm_utc;
    memset(&tm_utc, 0, sizeof(tm_utc));
    tm_utc.tm_sec  = sec;
    tm_utc.tm_min  = min;
    tm_utc.tm_hour = hour;
    tm_utc.tm_mday = day;
    tm_utc.tm_mon  = mon;
    tm_utc.tm_year = year - 1900;

    /* Convert UTC struct tm to UTC epoch.
     * mktime() interprets its argument as local time (per TZ), so we
     * must use timegm() or equivalent to get the correct UTC epoch.
     * NuttX provides timegm(); if unavailable, temporarily override TZ. */
#ifdef __NuttX__
    time_t t = timegm(&tm_utc);
#else
    /* Portable fallback: save TZ, set to UTC, call mktime, restore */
    char *old_tz = getenv("TZ");
    char  saved_tz[64] = {0};
    if (old_tz) strncpy(saved_tz, old_tz, sizeof(saved_tz) - 1);
    setenv("TZ", "UTC0", 1);
    tzset();
    time_t t = mktime(&tm_utc);
    if (old_tz) setenv("TZ", saved_tz, 1);
    else        unsetenv("TZ");
    tzset();
#endif
    if (t < 0) return false;

    /* v106：与 time_sync 的约定一致——RTC 里存本地时间（UTC+8），
     * 不再存 UTC 再格式化时 +8（那样会和 time_sync 路径差 8 小时）。 */
    struct timeval tv = { .tv_sec = t + 8 * 3600, .tv_usec = 0 };
    settimeofday(&tv, NULL);

    format_time_cn(t + 8 * 3600, out, out_size);
    return true;
}

/* ── Proxy path ─────────────────────────────────────────────── */

static int fetch_time_via_proxy(char *out, size_t out_size)
{
    proxy_conn_t *conn = proxy_conn_open("www.baidu.com", 443, 10000);
    if (!conn) return ERROR;

    const char *req =
        "HEAD / HTTP/1.1\r\n"
        "Host: www.baidu.com\r\n"
        "Connection: close\r\n\r\n";

    if (proxy_conn_write(conn, req, strlen(req)) < 0) {
        proxy_conn_close(conn);
        return ERROR;
    }

    char buf[1024];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int n = proxy_conn_read(conn, buf + total, sizeof(buf) - 1 - total, 10000);
        if (n <= 0) break;
        total += n;
        buf[total] = '\0';
        if (strstr(buf, "\r\n\r\n")) break;
    }
    proxy_conn_close(conn);

    char *date_hdr = strcasestr(buf, "\r\nDate: ");
    if (!date_hdr) return ERROR;
    date_hdr += 8; /* skip \r\nDate:  */

    char *eol = strstr(date_hdr, "\r\n");
    if (!eol) return ERROR;

    char date_val[64];
    size_t dlen = (size_t)(eol - date_hdr);
    if (dlen >= sizeof(date_val)) return ERROR;
    memcpy(date_val, date_hdr, dlen);
    date_val[dlen] = '\0';

    if (!parse_and_set_time(date_val, out, out_size)) return ERROR;
    return OK;
}

/* ── Direct path ──────────────────────────────────────────────── */

static int fetch_time_direct(char *out, size_t out_size)
{
    char date_val[64] = {0};
    int ret = vela_https_head_date("www.baidu.com", "443", "/", date_val, sizeof(date_val));
    if (ret != 0 || date_val[0] == '\0') return ERROR;
    if (!parse_and_set_time(date_val, out, out_size)) return ERROR;
    return OK;
}

/* ── Entry point ──────────────────────────────────────────────── */

int tool_get_time_execute(const char *input_json, char *output, size_t output_size)
{
    syslog(LOG_INFO, "[%s] Fetching current time...\n", TAG);

    /* Try local clock first — if year >= 2025, it's likely synced */
    time_t now = time(NULL);
    if (now > 1735689600) {  /* 2025-01-01 00:00:00 UTC */
        /* Use gmtime + manual offset to avoid zoneinfo lookup errors.
         * NuttX's localtime_r tries to open zoneinfo/<TZ> from romfs,
         * which fails for POSIX TZ strings like "CST-8" and spams
         * ERROR logs.  We bypass this by computing UTC+8 manually. */
        /* v106：time(NULL) 已经是本地时间（time_sync 已写入 +8），
         * 旧代码又 +8 导致多 8 小时；这里直接用 now 格式化。 */
        format_time_cn(now, output, output_size);
        syslog(LOG_INFO, "[%s] Time (local clock): %s\n", TAG, output);
        return OK;
    }

    /* Clock not set — fall back to HTTPS Date header */
    int err;
    if (http_proxy_is_enabled()) {
        err = fetch_time_via_proxy(output, output_size);
    } else {
        err = fetch_time_direct(output, output_size);
    }

    if (err == OK) {
        syslog(LOG_INFO, "[%s] Time (network): %s\n", TAG, output);
    } else {
        snprintf(output, output_size, "Error: failed to fetch time (err=%d)", err);
        syslog(LOG_ERR, "[%s] %s\n", TAG, output);
    }

    return err;
}
