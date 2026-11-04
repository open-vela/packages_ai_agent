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
 * HTTP TTS backend for the Vela/NuttX AI Agent.
 *
 * Speaks through a user-operated, key-less PCM service.  The wire
 * contract is intentionally simple (plain HTTP/1.1, no TLS, no body):
 *
 *     GET <base_url>?text=<percent-encoded UTF-8>&rate=16000
 *
 * The server answers with a raw PCM body: 24kHz, mono, signed 16-bit
 * little-endian (audio/L16).  We stream the body to the supplied
 * callback in 2-4KB chunks so playback starts before the full clip is
 * downloaded, then close on EOF (Connection: close).
 */

#include "voice/http_tts.h"

#include "agent_compat.h"
#include "agent_config.h"
#include "infra/config_store.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define HTTP_TTS_TAG "http_tts"

#ifndef CONFIG_AI_AGENT_TTS_HTTP_URL
/* [LOCAL-TTS 2026-10-27] 原来的 172.16.88.1 是编译机，板子跨网段够不到（实测 ping 不通）。
 * 192.168.110.25 是用户 PC，与板子同网段，跑 tts_server.py。 */
#define CONFIG_AI_AGENT_TTS_HTTP_URL "http://192.168.31.239:2301/tts"
#endif

/* config.json override key */
#define HTTP_TTS_CFG_KEY "tts_http_url"

#define HTTP_TTS_URL_MAX  96   /* [LOCAL-TTS 2026-10-27] was 256; dram0_0_seg 只差 16 B，URL 实际 31 字符，96 足够 */
#define HTTP_TTS_HDR_MAX  4096
#define HTTP_TTS_CHUNK    3072   /* stream chunk size (2-4KB range) */
#define HTTP_TTS_CONN_MS  1500  /* [LOCAL-TTS 2026-10-27] 服务离线时快速放弃 */   /* [V315] fail fast: 15s*4 left the voice
                                    * thread blocked for minutes when the
                                    * server is unreachable; 4s*2 still
                                    * covers the ~1s first-packet warm-up */
#define HTTP_TTS_RECV_MS  8000   /* per-recv timeout */
#define HTTP_TTS_CONN_RETRIES  5  /* [LOCAL-TTS 2026-10-28] 1->5: survive LwIP TIME_WAIT connect refusal */
#define HTTP_TTS_CONN_RETRY_MS 800  /* backoff between connect attempts */

/* [V357] send() on this board has been measured failing with ENOMEM (errno 12)
 * when the network stack has no free socket buffer at that instant.  The old
 * code gave up on the first error and the whole speak fell back to the
 * public MiMo backend (which PBR black-holes), so the ring looked like
 * "no network".  Retry the transient errno values before failing. */
#define HTTP_TTS_SEND_RETRIES  8
#define HTTP_TTS_SEND_RETRY_MS 150

/* [V68] /diag endpoint on the TTS bridge: log-only, tiny reply. */

#ifndef HTTP_DIAG_HOST
/* [V361] 原来指向 127.0.0.1（板子自己），每次 diag 都要白等
 * 300ms 建连超时；改成真正的 TTS bridge 所在主机。 */
#  define HTTP_DIAG_HOST "172.16.88.1"
#endif

#ifndef HTTP_DIAG_PORT
#  define HTTP_DIAG_PORT 2301
#endif

/* [V365] Diagnostics beacons are debug-only and, measured on V364,
 * they were competing with TTS for exactly the buffers TTS needs:
 * every diag_report() costs one TCP connection slot, one send() IOB
 * chain and 300 ms of connect budget.  The V364 log shows a 91-byte
 * beacon failing with
 *     [V360] [http_tts] EIO#1 (diag send) errno=12 req=91
 * i.e. the pool was already dry.  vc_beacon() and every B7_* probe
 * funnel through diag_report(), so gating this one function removes
 * all of them.
 *
 * Default OFF.  Compile with -DHTTP_DIAG_ENABLE=1 to bring the
 * /diag beacons back.
 */
#ifndef HTTP_DIAG_ENABLE
#  define HTTP_DIAG_ENABLE 0
#endif

#define HTTP_DIAG_CONN_MS        300

/* Tolerated consecutive empty receive windows (500 ms each) while waiting
 * for the bridge to synthesise or stream: ~20 s. */

#define HTTP_TTS_EAGAIN_RETRIES  40

/* [V361] 收响应头的重试预算单独收紧：7 x 500ms = 3.5s。
 * 服务器实测首字节 0.31~0.41s；旧的 41 x 500ms = 20.5s 只会把
 * “connect 假成功”的代价放大 20 倍。收 body 的预算保持 40（流式有空隙）。 */
#define HTTP_TTS_HDR_EAGAIN_RETRIES  30

/* [V361] 整条 TTS 请求（每次换全新 socket）最多试 3 次，间隔 150ms。 */
#define HTTP_TTS_MAX_ATTEMPTS       3
#define HTTP_TTS_ATTEMPT_BACKOFF_MS 150



static const char *TAG = HTTP_TTS_TAG;

/* [TTS-ABORT 2026-10-28] 允许 voice_channel 在打断旧 TTS 时，从另一个线程
 * 立刻关掉 http_tts 线程正阻塞的 socket，使 recv() 立即报错退出、连接即时
 * 释放，避免被放弃的句子一直占着连接直到服务器把整段（已放弃的）PCM 发完
 * （LwIP 资源耗尽 -> 新连接收不到 body -> EIO(-5) -> 静音）。 */
static volatile int      s_http_tts_abort_gen = -1; /* [TTS-ABORT-GEN] 被放弃的 speak 代数；-1=无 */
static int               s_http_tts_fd    = -1;  /* 正在运行的 speak 持有的活动 fd */
static volatile unsigned s_http_tts_gen     = 0;  /* [TTS-ABORT-GEN] speak 代数计数器 */
static volatile unsigned s_http_tts_fd_gen  = 0;  /* s_http_tts_fd 所属代 */

/* 安全关闭：fd 号 + 所属代双重匹配才真正 close。
 * [TTS-ABORT-GEN 2026-10-29] 旧实现只比 fd 号：旧 speak 退出清理时它的 fd 号
 * 可能已被新 speak 的 socket 复用，会把新 speak 的连接误关掉
 * （实测 recv(header):16 EBUSY -> -EIO(-5) -> 回答静音）。 */
#define HTTP_TTS_SAFE_CLOSE(fdp, gen) \
    do { \
        if (*(fdp) >= 0 && *(fdp) == s_http_tts_fd && \
            s_http_tts_fd_gen == (unsigned)(gen)) { \
            close(*(fdp)); \
            s_http_tts_fd = -1; \
        } \
        *(fdp) = -1; \
    } while (0)

/* 从另一线程调用：放弃当前正在进行的 speak，立即关闭其 socket。 */
void http_tts_abort_active(void)
{
    int f = s_http_tts_fd;

    s_http_tts_abort_gen = (int)s_http_tts_gen;  /* [TTS-ABORT-GEN] 只标记当前代 */
    if (f >= 0) {
        close(f);   /* 让阻塞在 recv() 的 speak 线程立即报错退出 */
        s_http_tts_fd = -1;
    }
}

/* Base URL, resolved at init() time. */
static char s_url[HTTP_TTS_URL_MAX] = CONFIG_AI_AGENT_TTS_HTTP_URL;

/* ── Config ──────────────────────────────────────────────── */

int http_tts_init(void)
{
    char buf[HTTP_TTS_URL_MAX];

    if (claw_config_get(HTTP_TTS_CFG_KEY, buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        strncpy(s_url, buf, sizeof(s_url) - 1);
        s_url[sizeof(s_url) - 1] = '\0';
        syslog(LOG_INFO, "[%s] base URL from config: %s\n", TAG, s_url);
    } else {
        syslog(LOG_INFO, "[%s] base URL from kconfig: %s\n", TAG, s_url);
    }

    return 0;
}

void http_tts_set_url(const char *url)
{
    if (!url) {
        return;
    }

    strncpy(s_url, url, sizeof(s_url) - 1);
    s_url[sizeof(s_url) - 1] = '\0';
}

/* ── URL helpers ─────────────────────────────────────────── */

/* Percent-encode UTF-8 text (RFC 3986 unreserved set kept literal). */
static int url_encode(const char *src, char *dst, size_t dst_cap)
{
    static const char hex[] = "0123456789ABCDEF";

    size_t di = 0;
    for (size_t si = 0; src[si]; si++) {
        unsigned char c = (unsigned char)src[si];

        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            if (di + 2 > dst_cap) {
                return -1;
            }
            dst[di++] = (char)c;
        } else {
            if (di + 4 > dst_cap) {
                return -1;
            }
            dst[di++] = '%';
            dst[di++] = hex[(c >> 4) & 0x0F];
            dst[di++] = hex[c & 0x0F];
        }
    }

    if (di + 1 > dst_cap) {
        return -1;
    }
    dst[di] = '\0';
    return 0;
}

/* Parse "http://host:port/path" into its parts. */
static int parse_url(const char *url, char *host, size_t host_cap,
                     int *port, char *path, size_t path_cap)
{
    const char *p = url;

    if (strncmp(p, "http://", 7) == 0) {
        p += 7;
    } else if (strncmp(p, "https://", 8) == 0) {
        p += 8;
    }

    const char *colon = strchr(p, ':');
    const char *slash = strchr(p, '/');
    const char *host_end = colon ? colon : (slash ? slash : p + strlen(p));

    size_t hlen = (size_t)(host_end - p);
    if (hlen == 0 || hlen >= host_cap) {
        return -1;
    }
    memcpy(host, p, hlen);
    host[hlen] = '\0';

    if (colon) {
        *port = atoi(colon + 1);
        p = colon + 1;
        while (*p >= '0' && *p <= '9') {
            p++;
        }
    } else {
        *port = 80;
    }

    if (*p == '/') {
        strncpy(path, p, path_cap - 1);
        path[path_cap - 1] = '\0';
    } else {
        path[0] = '/';
        path[1] = '\0';
    }

    return 0;
}

/* connect() with a deadline (non-blocking + select).
 *
 * NuttX/LwIP may report the socket as writable via select() BEFORE the TCP
 * handshake has actually completed, so getsockopt(SO_ERROR) can still return
 * EINPROGRESS at that point.  We therefore keep polling SO_ERROR until it is
 * definitively 0 (connected) or a real error, bounded by the deadline.  This
 * fixes the "connect ... failed: 115" race that otherwise breaks every TTS
 * request issued during the early-boot EINPROGRESS window. */
/* [V323] 有界连接等待 —— 完全不经过 select()/poll()。
 *
 * 移植自小智（ESP-IDF + esp_http_client）的连接范式：连接等待用定步长轮询 +
 * 硬性 deadline，不依赖 select() 的"可写"通知。
 *
 * 本项目已多次记录 select() 在本 NuttX 上不可靠："[V98C] connect select=0"
 * 在串口反复出现，而所有 "connect ... failed: 110(ETIMEDOUT)" 的报告都出自
 * select 版等待。
 *
 * 做法：非阻塞 connect 发出后只轮询 getsockopt(SO_ERROR)——
 *   err==0            -> 已连接
 *   err==EINPROGRESS  -> sleep 50ms 继续，直到 timeout_ms 用尽
 *   其它              -> 硬错误，立即返回
 * 返回 0 = 已连接；负数 = -errno（-ETIMEDOUT 表示用尽 deadline）。
 */
static int vc_bounded_wait(int fd, int timeout_ms)
{
    int waited = 0;
    const int step_ms = 50;

    while (waited < timeout_ms)
    {
        int err = 0;
        socklen_t elen = sizeof(err);
        unsigned char probe;

        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0)
        {
            int e = errno;

            syslog(LOG_ERR, "[VCONN] fd=%d getsockopt failed errno=%d\n", fd, e);
            return -e;
        }

        /* 只有"非 0 且非 EINPROGRESS"才是硬错误。
         * [V328] 注意：SO_ERROR==0 只表示"目前无挂起错误"，
         * **不代表已连接** —— 连接仍在进行时它同样返回 0。 */

        if (err != 0 && err != EINPROGRESS)
        {
            syslog(LOG_ERR,
                   "[VCONN] fd=%d hard error errno=%d after %dms (no select)\n",
                   fd, err, waited);
            return -err;
        }

        /* [V361] 真正的“已连接”判定：用 recv(MSG_PEEK|MSG_DONTWAIT) 探 1 字节。
         *
         * 为什么不再用 getpeername()：NuttX 在 connect() 发出 SYN 后就把对端
         * 地址写进了 conn，getpeername() 立刻返回 0 —— 三次握手还没完成就报
         * “连接成功”。这就是 [V360] 日志里 connect 假成功、随后 recv 头部空等
         * 41 x 500ms = 20.5s 的根源。
         *
         * 为什么不用重复 connect() 判定：本 NuttX 下 inet_connect() 对已连接
         * 套接字返回 -EISCONN，而 tcp_connect() 对“握手中”
         * (tcpstateflags != TCP_ALLOCATED) 同样返回 -EISCONN —— 两种情况
         * 无法区分。
         *
         * tcp_recvfrom() 的语义刚好可用：
         *   未连接        -> -ENOTCONN          （继续等）
         *   已连接、无数据 -> -EAGAIN/-EWOULDBLOCK（判定成功）
         *   有数据/对端关闭 -> >= 0              （判定成功）
         */

        probe = 0;
        if (recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT) >= 0)
        {
            syslog(LOG_ERR,
                   "[VCONN] fd=%d CONNECTED after %dms (probe: data/EOF)\n",
                   fd, waited);
            return 0;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT)
        {
            syslog(LOG_ERR,
                   "[VCONN] fd=%d CONNECTED after %dms (probe: EAGAIN)\n",
                   fd, waited);
            return 0;
        }

        if (errno != ENOTCONN && errno != EINPROGRESS && errno != EALREADY)
        {
            int e = errno;

            syslog(LOG_ERR,
                   "[VCONN] fd=%d probe hard error errno=%d after %dms\n",
                   fd, e, waited);
            return -e;
        }

        usleep(step_ms * 1000);
        waited += step_ms;
    }

    syslog(LOG_ERR, "[VCONN] fd=%d still connecting after %dms\n",
           fd, timeout_ms);
    return -ETIMEDOUT;
}

static int connect_timeout(int fd, const struct sockaddr *sa,
                           socklen_t len, int timeout_ms)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (connect(fd, sa, len) == 0) {
        fcntl(fd, F_SETFL, flags);
        return 0;
    }
    if (errno != EINPROGRESS) {
        fcntl(fd, F_SETFL, flags);
        return -1;
    }

    /* [V323] 移植小智的连接范式：不依赖 select() 的有界等待 */

    {
        int r = vc_bounded_wait(fd, timeout_ms);

        fcntl(fd, F_SETFL, flags);

        if (r < 0) {
            errno = -r;
            return -1;
        }

        return 0;
    }
}


/* [V68] Lightweight diagnostics channel.
 *
 * Probes used to travel as /tts requests, i.e. every probe tag made the
 * bridge synthesise speech and return a 30+ kB PCM body - slow, and it
 * exercised the very path it was meant to observe.  /diag only logs and
 * answers with two bytes, so probes are cheap enough to keep.
 */

int diag_report(const char *msg)
{
#if !HTTP_DIAG_ENABLE
    /* [V365] compiled out -- see HTTP_DIAG_ENABLE above. */
    (void)msg;
    return -ENOSYS;
#else
    struct sockaddr_in daddr;
    char req[256];
    char sink[64];
    int fd;
    int rl;
    int i;

    /* [V361] 退避闸门：连续失败后按 1s -> 2s -> 5s -> 10s -> 30s 抑制，
     * 不再让每个 TTS 事件都花 300ms 去建一次注定失败的连接。 */

    static unsigned int s_fail_streak;
    static time_t s_next_ok;

    if (msg == NULL || msg[0] == '\0') {
        return -EINVAL;
    }

    if (s_next_ok != 0)
        {
            if (time(NULL) < s_next_ok)
                {
                    return -EAGAIN;
                }
        }

    memset(&daddr, 0, sizeof(daddr));
    daddr.sin_family      = AF_INET;
    daddr.sin_port        = htons(HTTP_DIAG_PORT);
    daddr.sin_addr.s_addr = inet_addr(HTTP_DIAG_HOST);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -errno;
    }

    if (connect_timeout(fd, (struct sockaddr *)&daddr, sizeof(daddr),
                        HTTP_DIAG_CONN_MS) != 0) {
        {
            static const unsigned int backs[] = { 1, 2, 5, 10, 30 };
            unsigned int idx = s_fail_streak;

            if (idx >= sizeof(backs) / sizeof(backs[0]))
                {
                    idx = sizeof(backs) / sizeof(backs[0]) - 1;
                }

            s_fail_streak++;
            s_next_ok = time(NULL) + (time_t)backs[idx];
        }
        close(fd);
        return -ECONNREFUSED;
    }

    rl = snprintf(req, sizeof(req),
        "GET /diag?m=%s HTTP/1.0\r\n"
        "Host: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        msg, HTTP_DIAG_HOST);

    if (send(fd, req, (size_t)rl, 0) < 0) {
        
        {
            static const unsigned int backs[] = { 1, 2, 5, 10, 30 };
            unsigned int idx = s_fail_streak;

            if (idx >= sizeof(backs) / sizeof(backs[0]))
                {
                    idx = sizeof(backs) / sizeof(backs[0]) - 1;
                }

            s_fail_streak++;
            s_next_ok = time(NULL) + (time_t)backs[idx];
        }
        close(fd);
        return -EIO;
    }

    /* Drain a little so the peer is not reset mid-request; a stray EAGAIN
     * simply ends the drain. */

    for (i = 0; i < 4; i++) {
        if (recv(fd, sink, sizeof(sink), 0) <= 0) {
            break;
        }
    }

    s_fail_streak = 0;
    s_next_ok = 0;
    close(fd);
    return 0;
#endif /* HTTP_DIAG_ENABLE */
}

/* ── Streaming TTS ───────────────────────────────────────── */

/* [V360] 大小写不敏感地找 Content-Length。
 * 不用 strcasestr()（NuttX 不一定有）。返回 -1 = 没找到/解析失败。 */

static long v360_content_length(const char *hdr)
{
    static const char needle[] = "content-length:";
    const char *p = hdr;

    if (p == NULL)
        {
            return -1;
        }

    while (*p != '\0')
        {
            int i;
            int match = 1;

            for (i = 0; needle[i] != '\0'; i++)
                {
                    char c = p[i];

                    if (c >= 'A' && c <= 'Z')
                        {
                            c = (char)(c - 'A' + 'a');
                        }

                    if (c == '\0' || c != needle[i])
                        {
                            match = 0;
                            break;
                        }
                }

            if (match)
                {
                    return atol(p + i);
                }

            p++;
        }

    return -1;
}

/* [V361] 单次尝试中已交给播放器的音频字节数；用于判断能不能安全重试。 */
static int s_last_stream_bytes = 0;

static int http_tts_speak_stream_once(const char *text,
                          voice_tts_chunk_cb cb,
                          void *user_data)
{
    s_last_stream_bytes = 0;
    /* [TTS-ABORT-GEN 2026-10-29] 用代数替代全局 abort 布尔：新 speak 开新代，
     * 旧代的 abort 标志自然失效；旧 speak 清理时也因代数不匹配不会误关新 socket。 */
    unsigned mygen = ++s_http_tts_gen;

    if (!text || !cb) {
        return -EINVAL;
    }

    char host[128];
    char path[256];
    int port = 80;

    if (parse_url(s_url, host, sizeof(host), &port, path, sizeof(path)) != 0) {
        syslog(LOG_ERR, "[%s] bad base URL: %s\n", TAG, s_url);
        return -EINVAL;
    }

    /* Encode text (UTF-8 bytes, 3x worst case). */
    size_t enclen = strlen(text) * 3 + 1;
    char *enc = malloc(enclen);
    if (!enc) {
        return -ENOMEM;
    }
    if (url_encode(text, enc, enclen) != 0) {
        syslog(LOG_ERR, "[%s] text too long to encode\n", TAG);
        free(enc);
        return -E2BIG;
    }

    size_t rplen = strlen(path) + strlen(enc) + 32;
    char *req_path = malloc(rplen);
    char *req = NULL;
    if (!req_path) {
        free(enc);
        return -ENOMEM;
    }
    snprintf(req_path, rplen, "%s?text=%s&rate=16000", path, enc);

    /* [V360] 入口确认：证明这条 speak 走的是自建流式路径
     * （本文件自己的 socket + [V323]/[V328] connect_timeout，
     * **不是** vela_http_post_json()）。 */

    

    size_t reqlen = strlen(req_path) + 256;
    req = malloc(reqlen);
    if (!req) {
        free(enc);
        free(req_path);
        return -ENOMEM;
    }

    /* Resolve host (literal IPv4 first, then DNS). */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    struct in_addr ip;
    if (inet_pton(AF_INET, host, &ip) == 1) {
        addr.sin_addr = ip;
    } else {
        struct hostent *he = gethostbyname(host);
        if (!he || !he->h_addr) {
            syslog(LOG_ERR, "[%s] cannot resolve host: %s\n", TAG, host);
            free(enc);
            free(req_path);
            free(req);
            return -ENOENT;
        }
        memcpy(&addr.sin_addr, he->h_addr, sizeof(struct in_addr));
    }

    /* NuttX/LwIP can refuse the 2nd..Nth outbound connection to the same
     * server while the previous PCB still lingers (TIME_WAIT) or the
     * ephemeral port is still bound: connect() fails and no SYN is even
     * emitted.  Retry with a short backoff instead of losing the answer. */
    int fd = -1;

    for (int attempt = 0; attempt < HTTP_TTS_CONN_RETRIES; attempt++) {
        if (s_http_tts_abort_gen == (int)mygen) {   /* [TTS-ABORT-GEN] 连接阶段也被打断 */
            free(enc);
            free(req_path);
            free(req);
            return -ECONNABORTED;
        }
        if (attempt > 0) {
            usleep(HTTP_TTS_CONN_RETRY_MS * 1000);
        }

        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            syslog(LOG_ERR, "[%s] socket(): %d\n", TAG, errno);
            continue;
        }

        /* Reuse the local port even if a previous connection to this server
         * is still in TIME_WAIT (needs CONFIG_NET_TCPPROTO_OPTIONS=y). */
        int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        /* Per-recv timeout so a stalled server cannot hang playback. */
        struct timeval rtv;
        rtv.tv_sec = HTTP_TTS_RECV_MS / 1000;
        rtv.tv_usec = (HTTP_TTS_RECV_MS % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));

        if (connect_timeout(fd, (struct sockaddr *)&addr, sizeof(addr),
                            HTTP_TTS_CONN_MS) == 0) {
            break;
        }

        syslog(LOG_WARNING, "[%s] connect %s:%d attempt %d/%d failed: %d\n",
            TAG, host, port, attempt + 1, HTTP_TTS_CONN_RETRIES, errno);
        HTTP_TTS_SAFE_CLOSE(&fd, mygen);
    }

    if (fd < 0) {
        syslog(LOG_ERR, "[%s] connect %s:%d gave up after %d attempts\n",
            TAG, host, port, HTTP_TTS_CONN_RETRIES);
        free(enc);
        free(req_path);
        free(req);
        return -ECONNREFUSED;
    }

    s_http_tts_fd = fd;          /* [TTS-ABORT] 活动 fd 登记，供 abort 从另一线程关闭 */
    s_http_tts_fd_gen = mygen;   /* [TTS-ABORT-GEN] 登记该 fd 所属代 */


    /* [V68] Poll instead of relying on the 8 s SO_RCVTIMEO: the slower
     * timeout cannot be retried cheaply, while a 500 ms window lets recv()
     * ride out an empty stretch without aborting the fetch. */

    {
        struct timeval ptv;
        int fl;

        ptv.tv_sec  = 0;
        ptv.tv_usec = 500 * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &ptv, sizeof(ptv));

        /* Make sure the socket really is blocking: SO_RCVTIMEO does nothing
         * on a non-blocking socket. */

        fl = fcntl(fd, F_GETFL, 0);
        if (fl >= 0) {
            fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
        }
    }

    int rl = snprintf(req, reqlen,
        "GET %s HTTP/1.1\r\n"
        "Host: %s:%d\r\n"
        "User-Agent: ai_agent/1.0\r\n"
        "Accept: */*\r\n"
        "Connection: close\r\n"
        "\r\n",
        req_path, host, port);

    {
        int sent     = -1;
        int last_err = 0;
        int attempt  = 0;

        for (attempt = 0; attempt < HTTP_TTS_SEND_RETRIES; attempt++)
          {
            sent = send(fd, req, (size_t)rl, 0);
            if (sent >= 0)
              {
                break;
              }

            last_err = errno;
            if (last_err != EAGAIN &&
                last_err != ENOMEM &&
                last_err != EINTR)
              {
                break;
              }

            usleep(HTTP_TTS_SEND_RETRY_MS * 1000);
          }

        if (sent < 0)
          {
            
            diag_report("TTS_SEND_FAIL");

            HTTP_TTS_SAFE_CLOSE(&fd, mygen);
            free(enc);
            free(req_path);
            free(req);
            return -EIO;
          }

        if (attempt > 0)
          {
            
          }
    }

    /* Read the header section, then stream the body. */
    char hdr[HTTP_TTS_HDR_MAX];
    int hdr_len = 0;

    /* [V360] 保证失败路径打印 hdr80 时不会输出栈上未初始化的内容。 */
    hdr[0] = '\0';
    bool header_done = false;
    int hdr_retries = 0;
    static int s_v68_hdr_seen;

    while (!header_done) {
        int n = recv(fd, hdr + hdr_len, sizeof(hdr) - hdr_len - 1, 0);

        /* [V68] An empty window while the bridge synthesises is normal. */

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                      errno == ETIMEDOUT) &&
            ++hdr_retries <= HTTP_TTS_HDR_EAGAIN_RETRIES) {
            continue;
        }

        if (n < 0) {
            if (s_http_tts_abort_gen == (int)mygen) {   /* [TTS-ABORT-GEN] 被打断，fd 已由另一线程关 */
                HTTP_TTS_SAFE_CLOSE(&fd, mygen);
                free(enc);
                free(req_path);
                free(req);
                return -ECONNABORTED;
            }
            diag_report("TTS_HDR_FAIL");
            syslog(LOG_ERR, "[%s] recv(header): %d\n", TAG, errno);

            HTTP_TTS_SAFE_CLOSE(&fd, mygen);
            free(enc);
            free(req_path);
            free(req);
            return -EIO;
        }
        if (n == 0) {
            syslog(LOG_ERR, "[%s] closed before full headers\n", TAG);

            HTTP_TTS_SAFE_CLOSE(&fd, mygen);
            free(enc);
            free(req_path);
            free(req);
            return -EIO;
        }

        hdr_len += n;
        hdr[hdr_len] = '\0';

        char *sep = strstr(hdr, "\r\n\r\n");
        if (sep) {
            int status = 0;
            if (strncmp(hdr, "HTTP/", 5) == 0) {
                status = atoi(hdr + 9); /* skip "HTTP/x.x " */
            }
            if (status < 200 || status >= 300) {
                syslog(LOG_ERR, "[%s] HTTP %d from TTS service\n", TAG, status);
                HTTP_TTS_SAFE_CLOSE(&fd, mygen);
                free(enc);
                free(req_path);
                free(req);
                return -EPROTO;
            }

            char *body_start = sep + 4;
            int leftover = hdr_len - (int)(body_start - hdr);
            if (leftover > 0) {
                cb((const unsigned char *)body_start, (size_t)leftover,
                   0, user_data);
                s_last_stream_bytes += leftover;
            }
            header_done = true;

            if (!s_v68_hdr_seen) {
                s_v68_hdr_seen = 1;
                diag_report("TTS_HDR_OK");
            }

            /* [V360] 头拿到了：把 http 状态、头长度、Content-Length、
             * 头原文前 80 字节全部打出来。 */

            
        }

        if (hdr_len >= (int)sizeof(hdr) - 1) {
            syslog(LOG_ERR, "[%s] header section too large\n", TAG);

            HTTP_TTS_SAFE_CLOSE(&fd, mygen);
            free(enc);
            free(req_path);
            free(req);
            return -EIO;
        }
    }

    /* Stream body until the server closes the connection. */
    char body[HTTP_TTS_CHUNK];
    int body_bytes = 0;
    int body_idle = 0;
    static int s_v68_body_seen;

    for (;;) {
        int n = recv(fd, body, sizeof(body), 0);

        /* [V68] Ride out empty windows instead of aborting on the first one.
         * Give up only after a long idle stretch, and still play whatever
         * body already arrived. */

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (s_http_tts_abort_gen == (int)mygen) {   /* [TTS-ABORT-GEN] 被打断，fd 已由另一线程关 */
                break;
            }
            if (++body_idle <= HTTP_TTS_EAGAIN_RETRIES) {
                continue;
            }

            if (body_bytes == 0) {
                diag_report("TTS_BODY_NODATA");
                syslog(LOG_ERR, "[%s] recv() timed out (no body)\n", TAG);
                HTTP_TTS_SAFE_CLOSE(&fd, mygen);
                free(enc);
                free(req_path);
                free(req);
                return -ETIMEDOUT;
            }

            break;
        }

        if (n < 0) {
            if (s_http_tts_abort_gen == (int)mygen) {   /* [TTS-ABORT-GEN] 被打断，fd 已由另一线程关 */
                HTTP_TTS_SAFE_CLOSE(&fd, mygen);
                free(enc);
                free(req_path);
                free(req);
                return -ECONNABORTED;
            }
            diag_report("TTS_BODY_FAIL");
            syslog(LOG_ERR, "[%s] recv(body): %d\n", TAG, errno);

            HTTP_TTS_SAFE_CLOSE(&fd, mygen);
            free(enc);
            free(req_path);
            free(req);
            return -EIO;
        }
        if (n == 0) {
            break; /* EOF: Connection: close */
        }

        body_bytes += n;
        body_idle = 0;

        if (!s_v68_body_seen) {
            s_v68_body_seen = 1;
            diag_report("TTS_BODY_OK");
        }

        cb((const unsigned char *)body, (size_t)n, 0, user_data);
        s_last_stream_bytes += n;
    }

    /* [V357] The "TTS audio bytes" ring: one verifiable line per call. */
    

    if (body_bytes == 0) {
        diag_report("TTS_BODY_EMPTY");
    }

    /* Final (empty) chunk marker. */
    cb(NULL, 0, 1, user_data);

    HTTP_TTS_SAFE_CLOSE(&fd, mygen);
    free(enc);
    free(req_path);
    free(req);
    return 0;
}

/* [V361] 对外仍是 http_tts_speak_stream()：整条请求最多 3 次，每次换全新
 * socket。只有在“一个字节都没播出去”时才重试，避免重复播放同一段音频。 */

/* [ONE-VOICE 2026-10-27] 探测本地 TTS 服务是否可达。
 *
 * 只做 TCP connect，不发请求、不等响应，同网段 20 ms 内返回。
 * aivox 在 tts/start 时用它决定这一句是"用本地音色重念"还是"照旧播
 * 服务端音频"：服务不在就走服务端音频，绝不把云端回答变成哑巴。 */
int http_tts_ping(void)
{
    char host[128];
    char path[256];
    int  port = 80;
    int  fd;
    struct sockaddr_in addr;
    struct in_addr     ip;

    if (parse_url(s_url, host, sizeof(host), &port, path, sizeof(path)) != 0)
        {
            return -EINVAL;
        }

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);

    if (inet_pton(AF_INET, host, &ip) == 1)
        {
            addr.sin_addr = ip;
        }
    else
        {
            struct hostent *he = gethostbyname(host);

            if (he == NULL || he->h_addr == NULL)
                {
                    return -ENOENT;
                }

            memcpy(&addr.sin_addr, he->h_addr, sizeof(struct in_addr));
        }

    /* [PINGRETRY 2026-10-30] 单次探测是误判源：PC 端 edge-tts 正在给其他
     * 连接做流式合成时，accept/SYN 可能短暂积压，一次超时就把整个回答
     * 降级播云端男声（实测"一下女声一下大叔声"）。改 3 次探测（间隔
     * 150ms）：健康时首次即成功零开销；真宕机最多多花 ~1.7s 才降级。 */
    for (int attempt = 0; attempt < 3; attempt++)
        {
            fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0)
                {
                    syslog(LOG_ERR, "[http_tts] ping socket(): %d\n", errno);
                    return -EIO;
                }

            if (connect_timeout(fd, (struct sockaddr *)&addr,
                                sizeof(addr), 800) == 0)
                {
                    close(fd);
                    return 0;
                }

            syslog(LOG_WARNING,
                   "[http_tts] ping attempt %d/3 failed: %d\n",
                   attempt + 1, errno);
            close(fd);

            if (attempt < 2)
                {
                    usleep(150000);
                }
        }

    return -ETIMEDOUT;
}

int http_tts_speak_stream(const char *text,
                          voice_tts_chunk_cb cb,
                          void *user_data)
{
    int attempt;
    int ret = -EIO;

    for (attempt = 1; attempt <= HTTP_TTS_MAX_ATTEMPTS; attempt++)
        {
            ret = http_tts_speak_stream_once(text, cb, user_data);

            if (ret == 0 || ret == -ECONNABORTED)
                {
                    if (attempt > 1 && ret == 0)
                        {

                        }
                    break;
                }

            

            /* 已经吐出去音频了：再试会重复播放，直接放弃。 */
            if (s_last_stream_bytes > 0)
                {
                    break;
                }

            if (attempt < HTTP_TTS_MAX_ATTEMPTS)
                {
                    usleep(HTTP_TTS_ATTEMPT_BACKOFF_MS * 1000);
                }
        }

    return ret;
}
