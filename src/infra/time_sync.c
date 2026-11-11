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
 * time_sync.c — 把系统时钟对齐到网络时间(SNTP)。
 *
 * 为什么需要应用自己做:AP 没有 netdev(网络走 usrsock 到 CP),
 * apps/netutils/netinit 的 ntpc_start() 挂在 NETINIT_HAVE_NETDEV 分支里,
 * 开机根本执行不到;而 Wi-Fi 又是用户后敲 `wapi connect` 才起来的。于是
 * 时钟停在 1970,待机时钟的日期/时间全错,HTTPS 证书也会因时钟越界失效。
 *
 * 这里直接发标准 SNTP 请求(48 字节 UDP,模式 3),取服务器 transmit
 * timestamp 后 settimeofday()。相比 ntpc 守护的好处是每一步失败都有
 * 明确的 errno 日志,真机上(控制台会丢字)也能定位是 DNS 还是 udp/123。
 * 成功后每 RESYNC_SEC 再校一次,抵消晶振漂移。
 */

#include <nuttx/config.h>

#include "time_sync.h"
#include "network_manager.h"
#include "agent_compat.h"

#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "time_sync";

#define SNTP_SERVER_PORT   "123"
#define SNTP_RECV_TIMEOUT  5    /* per-server UDP timeout */
#define SNTP_RETRY_SEC     20   /* retry cadence while unsynced */
#define SNTP_RESYNC_SEC    1800 /* re-sync cadence once synced */
#define SNTP_PACKET_LEN    48
#define NTP_UNIX_DELTA     2208988800ULL /* 1900-01-01 -> 1970-01-01 */

#define TIME_SYNC_STACK 4096
#define TIME_SYNC_PRIO  100

/* Public NTP servers; the board's corporate DNS (set_dns 10.237.25.8)
 * resolves these even though outbound udp/123 may still be filtered. */
static const char* s_servers[] = {
    "ntp1.aliyun.com",
    "ntp.aliyun.com",
    "ntp1.ntsc.ac.cn",
    NULL
};

static volatile bool s_synced;
static char s_last_err[128] = "not attempted";

/* Serialises the daemon's periodic sync against an on-demand one. */
static pthread_mutex_t s_sync_lock = PTHREAD_MUTEX_INITIALIZER;
static bool s_started;

static uint32_t be32(const unsigned char* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16)
        | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* One SNTP request.  Returns 0 and sets *out to the server's transmit time
 * (Unix epoch) on success; otherwise -1 with a human-readable reason in err.
 */
static int sntp_query(const char* host, time_t* out, char* err, size_t err_len)
{
    struct addrinfo hints;
    struct addrinfo* res = NULL;
    unsigned char pkt[SNTP_PACKET_LEN];
    unsigned char resp[SNTP_PACKET_LEN];
    struct timeval tv;
    int fd = -1;
    int rc;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;

    rc = getaddrinfo(host, SNTP_SERVER_PORT, &hints, &res);
    if (rc != 0 || res == NULL) {
        snprintf(err, err_len, "resolve %s failed (%d)", host, rc);
        return -1;
    }

    fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        snprintf(err, err_len, "socket failed (errno=%d)", errno);
        freeaddrinfo(res);
        return -1;
    }

    tv.tv_sec = SNTP_RECV_TIMEOUT;
    tv.tv_usec = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* LI=0, VN=4, Mode=3 (client).  Everything else zero. */
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = (0 << 6) | (4 << 3) | 3;

    rc = sendto(fd, pkt, sizeof(pkt), 0, res->ai_addr, res->ai_addrlen);
    if (rc < 0) {
        snprintf(err, err_len, "%s sendto failed (errno=%d)", host, errno);
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    rc = recv(fd, resp, sizeof(resp), 0);
    if (rc < SNTP_PACKET_LEN) {
        snprintf(err, err_len, "%s recv %d bytes (errno=%d; udp/123 blocked?)",
            host, rc, errno);
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    close(fd);
    freeaddrinfo(res);

    /* Server reply: mode 4 (server) or 5 (broadcast). */
    if ((resp[0] & 0x7) != 4 && (resp[0] & 0x7) != 5) {
        snprintf(err, err_len, "%s bad reply mode %d", host, resp[0] & 0x7);
        return -1;
    }

    /* Transmit timestamp = bytes 40..47; seconds are the first 4. */
    uint32_t secs = be32(resp + 40);
    if (secs < NTP_UNIX_DELTA) {
        snprintf(err, err_len, "%s invalid timestamp %lu", host,
            (unsigned long)secs);
        return -1;
    }

    *out = (time_t)((uint64_t)secs - NTP_UNIX_DELTA);
    return 0;
}

static bool sntp_sync_once(void)
{
    time_t t = 0;

    for (int i = 0; s_servers[i]; i++) {
        if (sntp_query(s_servers[i], &t, s_last_err, sizeof(s_last_err)) == 0) {
            struct timeval tv = { .tv_sec = t, .tv_usec = 0 };
            if (settimeofday(&tv, NULL) != 0) {
                snprintf(s_last_err, sizeof(s_last_err),
                    "settimeofday failed (errno=%d)", errno);
                return false;
            }
            syslog(LOG_INFO,
                "[%s] clock synced from %s: epoch=%lld\n",
                TAG, s_servers[i], (long long)t);
            return true;
        }
    }

    return false;
}

static void* time_sync_task(void* arg)
{
    (void)arg;

    while (1) {
        bool ok;

        if (!network_is_connected()) {
            sleep(5);
            continue;
        }

        pthread_mutex_lock(&s_sync_lock);
        ok = sntp_sync_once();
        pthread_mutex_unlock(&s_sync_lock);

        if (ok) {
            s_synced = true;
            sleep(SNTP_RESYNC_SEC);
        } else {
            syslog(LOG_WARNING, "[%s] SNTP failed: %s (retry in %ds)\n",
                TAG, s_last_err, SNTP_RETRY_SEC);
            sleep(SNTP_RETRY_SEC);
        }
    }

    return NULL;
}

int time_sync_now(char* err, size_t err_len)
{
    bool ok;

    if (!network_is_connected()) {
        if (err != NULL && err_len > 0) {
            snprintf(err, err_len, "network is down");
        }
        return -1;
    }

    /* One request at a time: the daemon may be awake at the same moment, and
     * two settimeofday() racing would make the printed result meaningless. */
    pthread_mutex_lock(&s_sync_lock);
    ok = sntp_sync_once();
    if (ok) {
        s_synced = true;
    } else if (err != NULL && err_len > 0) {
        snprintf(err, err_len, "%s", s_last_err);
    }
    pthread_mutex_unlock(&s_sync_lock);

    return ok ? 0 : -1;
}

bool time_sync_has_synced(void)
{
    return s_synced;
}

const char* time_sync_last_error(void)
{
    return s_last_err;
}

int time_sync_start(void)
{
    if (s_started) {
        return 0;
    }

    int ret = agent_task_create(time_sync_task, "time_sync",
        TIME_SYNC_STACK, NULL, TIME_SYNC_PRIO);
    if (ret != OK) {
        syslog(LOG_WARNING, "[%s] task create failed\n", TAG);
        return -1;
    }

    s_started = true;
    syslog(LOG_INFO, "[%s] supervisor started (SNTP after the link is up)\n",
        TAG);
    return 0;
}
