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

/* Difference between this board's clock and real time.
 *
 * The clock cannot simply be corrected. The RTC comes up reading 2036, and
 * stepping the system clock back ten years under a running system disturbs
 * every timeout already queued -- the network stack's, the message bus's,
 * cron's -- and the device stops answering shortly afterwards. That is what
 * happened the first time this code tried to fix the clock: the time request
 * succeeded and the board went unresponsive moments later.
 *
 * So the clock is left alone and the difference is remembered instead, then
 * applied wherever a time is reported. Timers keep running against the clock
 * they were armed with, and everything the user sees is right.
 *
 * Zero until a fetch has succeeded, so with no network the local clock is
 * reported untouched -- wrong, but not pretending otherwise.
 */
static time_t s_time_offset;
static bool   s_time_known;

time_t tool_get_time_offset(void)
{
    return s_time_known ? s_time_offset : 0;
}

static void apply_network_time(time_t epoch)
{
    s_time_offset = epoch - time(NULL);
    s_time_known  = true;

    syslog(LOG_INFO, "[%s] Board clock is off by %lld s; reporting corrected "
           "time without setting it\n", TAG, (long long)s_time_offset);
}

static const char *MONTHS[] = {
    "Jan","Feb","Mar","Apr","May","Jun",
    "Jul","Aug","Sep","Oct","Nov","Dec"
};

/* Parse "Sat, 01 Feb 2025 10:25:00 GMT" → set system clock, return formatted string */
static bool parse_and_set_time(const char *date_str, char *out, size_t out_size)
{
    int day, year, hour, min, sec;
    char mon_str[4] = {0};

    /* "Wed, 16 Sep 2026 14:46:39 GMT".
     *
     * The leading "Www," is dropped by hand rather than with a %*[^,]
     * conversion: that is an assignment-suppressed scanset, and this libc
     * does not implement it -- the scan simply fails, which made every fetch
     * look like a network failure while the reply in hand was perfect.
     * Everything after the comma is plain conversions, which do work.
     *
     * Found by having the failure print the reply it could not parse; that
     * is worth keeping in mind for the next one of these, since the syslog
     * this would normally be read from is not reachable.
     */
    const char *p = strchr(date_str, ',');

    if (sscanf(p ? p + 1 : date_str, " %d %3s %d %d:%d:%d",
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

    /* Record the difference rather than moving the clock. See s_time_offset
     * above: stepping a system clock that is ten years out, under a running
     * system, disturbs every timeout already queued and the device stops
     * responding shortly afterwards. */
    apply_network_time(t);

    /* Format in local time (TZ=CST-8 → UTC+8) using gmtime to avoid
     * zoneinfo lookup errors on NuttX romfs */
    struct tm local;
    time_t local_epoch = t + 8 * 3600;
    gmtime_r(&local_epoch, &local);

    char time_str[64];
    strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S (%A)", &local);
    snprintf(out, out_size, "%s CST (UTC+8), UNIX epoch: %lld", time_str, (long long)t);

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

/* ── Time from the configured backend ─────────────────────────── */

/* Ask the LLM backend for the time, over plain HTTP.
 *
 * The board's own HTTPS stack cannot be relied on to reach a public host: the
 * handshake against a CDN edge fails more often than it succeeds on this
 * link, and each failure tends to trip a NuttX semaphore assertion that stops
 * the device dead. The backend, by contrast, is reached over plain HTTP on
 * every single turn without trouble -- so it is both the easiest thing to ask
 * and the one most likely to answer.
 *
 * The reply body carries the date in the same RFC-1123 form the Date: header
 * uses, so the existing parser handles it as-is.
 */
static int fetch_time_from_backend(char *out, size_t out_size)
{
    char host[64] = {0};
    char port[8] = {0};
    char resp[128] = {0};

    if (claw_config_get(AGENT_CFG_KEY_LLM_HOST, host, sizeof(host)) != OK
        || host[0] == '\0') {
        return ERROR;
    }

    if (claw_config_get("llm_port", port, sizeof(port)) != OK
        || port[0] == '\0') {
        snprintf(port, sizeof(port), "80");
    }

    if (vela_http_post_json(host, port, "/time", NULL, "{}",
                            resp, sizeof(resp)) < 0) {
        syslog(LOG_ERR, "[%s] time request to %s:%s failed\n", TAG, host, port);
        return ERROR;
    }

    if (!parse_and_set_time(resp, out, out_size)) {
        syslog(LOG_ERR, "[%s] backend sent a date this build cannot parse: "
               "%.70s\n", TAG, resp);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Time (backend): %s\n", TAG, out);
    return OK;
}

/* ── Entry point ──────────────────────────────────────────────── */

int tool_get_time_execute(const char *input_json, char *output, size_t output_size)
{
    syslog(LOG_INFO, "[%s] Fetching current time...\n", TAG);

    /* Try local clock first — but only if it is plausible.
     *
     * A lower bound is not enough. This board's RTC comes up reading 2036,
     * which passes "after 2025" and is then trusted forever -- and because
     * this branch is the one place that would have fetched and corrected the
     * time, a clock that is wrong into the *future* can never repair itself.
     * Every answer then carries a date ten years out, on the watch screen and
     * in every report. Wrong by ten years is as wrong as wrong by fifty, so
     * the check is a window, not a floor.
     */
    time_t now = time(NULL);

    /* A known offset is as good as a correct clock, and costs nothing. */
    if (s_time_known) {
        now += s_time_offset;
    }

    if (now > 1735689600 && now < 1893456000) {  /* 2025-01-01 .. 2030-01-01 */
        /* Use gmtime + manual offset to avoid zoneinfo lookup errors.
         * NuttX's localtime_r tries to open zoneinfo/<TZ> from romfs,
         * which fails for POSIX TZ strings like "CST-8" and spams
         * ERROR logs.  We bypass this by computing UTC+8 manually. */
        struct tm utc_tm;
        time_t local_epoch = now + 8 * 3600;  /* UTC+8 */
        gmtime_r(&local_epoch, &utc_tm);

        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", &utc_tm);
        snprintf(output, output_size,
                 "%s CST (UTC+8), UNIX epoch: %lld",
                 time_str, (long long)now);
        syslog(LOG_INFO, "[%s] Time (local clock): %s\n", TAG, output);
        return OK;
    }

    /* Clock not set — fall back to HTTPS Date header */
    int err;
    if (http_proxy_is_enabled()) {
        err = fetch_time_via_proxy(output, output_size);
    } else {
        /* Backend first: it is local, it speaks plain HTTP, and it answers
         * every turn. The public-host HTTPS path stays as the fallback for
         * deployments whose backend is somewhere else. */
        err = fetch_time_from_backend(output, output_size);
        if (err != OK) {
            err = fetch_time_direct(output, output_size);
        }
    }

    if (err == OK) {
        syslog(LOG_INFO, "[%s] Time (network): %s\n", TAG, output);
    } else {
        snprintf(output, output_size, "Error: failed to fetch time (err=%d)", err);
        syslog(LOG_ERR, "[%s] %s\n", TAG, output);
    }

    return err;
}
