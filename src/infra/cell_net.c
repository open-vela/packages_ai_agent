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

/****************************************************************************
 * cell_net.c — 合宙 Air780E：AT 控制 + 用户态 pppd 数据链路
 *
 * 为什么这么写：
 *   1) 板级只把 UART2 (/dev/ttyS0) 引出来，AT 和 PPP 共用同一条串口。
 *      任何 AT 请求都必须和 pppd 互斥，否则 "AT" 字节会插进 LCP/IPCP 帧，
 *      表现是 PPP 偶发超时重连，很难查。这里用一把全局锁 + s_ppp_active
 *      标志：PPP 一旦被接受，AT 路径直接返回 -EBUSY。
 *   2) pppd() 是阻塞的，而且本树 app/netutils/pppd 内部是 while(1) 循环，
 *      正常情况永远不会返回（断线由 ppp_reconnect 自己重拨）。所以放到
 *      detached 线程里跑，主线程/CLI 不会被卡住。
 *   3) pppd 自己 open("/dev/ttyS0")，不是复用我们的 fd。因此“关掉我们的
 *      fd”只能让 AT 路径退出，不能打断 pppd —— cell_ppp_stop() 如实返回
 *      -EBUSY，而不是假装停止成功。
 ****************************************************************************/

#include <nuttx/config.h>

#include "infra/cell_net.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netutils/netlib.h>

#ifdef CONFIG_SERIAL_TERMIOS
#include <termios.h>
#endif

#ifdef CONFIG_NETUTILS_PPPD
#include <netutils/pppd.h>
#endif

#include "infra/config_store.h"

static const char* TAG = "cell";

#define CELL_TTY              "/dev/ttyS0"  /* UART2: PA20/PA27 */
#define CELL_APN_KEY          "net.apn"
#define CELL_APN_DEFAULT      "cmnet"
#define CELL_APN_MAX          64
#define CELL_AT_TIMEOUT_MS    1500
#define CELL_RSP_MAX          512
#define CELL_LAST_RSP_MAX     160         /* 状态里保留的最近响应字节数 */
#define CELL_PPP_STACK        8192
#define CELL_PPP_RETRY_MS     5000

#ifdef CONFIG_SERIAL_TERMIOS
#define CELL_BAUD             B115200
#endif

/* s_lock 保护 AT 事务、s_uart_fd、PPP 生命周期标志和 s_last_at。
 * s_apn_lock 只保护 APN 字符串，避免 AT 热路径和配置读写互相等待。 */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t s_apn_lock = PTHREAD_MUTEX_INITIALIZER;

static char s_apn[CELL_APN_MAX] = CELL_APN_DEFAULT;
static bool s_inited;

static int s_uart_fd = -1;
static int s_uart_ok;                    /* >0 上次 AT 成功, <0 失败, 0 未试 */
static char s_last_at[CELL_LAST_RSP_MAX + 1];

#ifdef CONFIG_NETUTILS_PPPD
static volatile int s_ppp_want;          /* 用户希望 PPP 一直在线 */
static volatile int s_ppp_running;       /* ppp 线程还活着 */
static volatile int s_ppp_active;        /* tty 已被 pppd 预定/占用 */
#endif

/* ── 小工具 ──────────────────────────────────────────────────── */

static uint64_t cell_now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0) {
        return 0;
    }

    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}

static int cell_apn_is_valid(const char *apn)
{
    size_t i;
    size_t len;

    if (apn == NULL) {
        return -EINVAL;
    }

    len = strlen(apn);
    if (len == 0 || len >= CELL_APN_MAX) {
        return -EINVAL;
    }

    /* APN 会原样嵌进 chat 脚本，空格/引号/反斜杠会把脚本拆坏。 */
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)apn[i];
        if (!isprint(c) || isspace(c) || c == '"' || c == '\\') {
            return -EINVAL;
        }
    }

    return 0;
}

static void cell_apn_copy(char *dst, size_t cap)
{
    if (dst == NULL || cap == 0) {
        return;
    }

    cell_net_init();

    pthread_mutex_lock(&s_apn_lock);
    strncpy(dst, s_apn, cap - 1);
    dst[cap - 1] = '\0';
    pthread_mutex_unlock(&s_apn_lock);
}

static void cell_sanitize(char *s)
{
    for (; *s != '\0'; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '\r' || c == '\n' || c == '\t') {
            *s = ' ';
        } else if (c < 0x20 || c > 0x7e) {
            *s = '.';
        }
    }
}

/* ── APN 配置 ────────────────────────────────────────────────── */

int cell_net_init(void)
{
    pthread_mutex_lock(&s_apn_lock);
    if (!s_inited) {
        char buf[CELL_APN_MAX];

        s_inited = true;

        if (claw_config_get(CELL_APN_KEY, buf, sizeof(buf)) == 0 &&
            cell_apn_is_valid(buf) == 0) {
            strncpy(s_apn, buf, sizeof(s_apn) - 1);
            s_apn[sizeof(s_apn) - 1] = '\0';
            syslog(LOG_INFO, "[%s] apn=%s (from %s)\n", TAG, s_apn,
                   CELL_APN_KEY);
        } else {
            syslog(LOG_INFO,
                   "[%s] apn=%s (default; %s missing/invalid)\n", TAG,
                   s_apn, CELL_APN_KEY);
        }

        syslog(LOG_INFO, "[%s] cell_net ready: tty=%s ppp=%s\n", TAG,
               CELL_TTY,
#ifdef CONFIG_NETUTILS_PPPD
               "enabled"
#else
               "disabled (CONFIG_NETUTILS_PPPD=n)"
#endif
        );
    }
    pthread_mutex_unlock(&s_apn_lock);

    return 0;
}

const char *cell_get_apn(void)
{
    cell_net_init();
    return s_apn;
}

int cell_set_apn(const char *apn)
{
    int rc;

    rc = cell_apn_is_valid(apn);
    if (rc < 0) {
        syslog(LOG_WARNING, "[%s] invalid apn\n", TAG);
        return rc;
    }

    if (claw_config_set(CELL_APN_KEY, apn) != 0) {
        syslog(LOG_ERR, "[%s] cannot persist %s\n", TAG, CELL_APN_KEY);
        return -EIO;
    }

    pthread_mutex_lock(&s_apn_lock);
    strncpy(s_apn, apn, sizeof(s_apn) - 1);
    s_apn[sizeof(s_apn) - 1] = '\0';
    pthread_mutex_unlock(&s_apn_lock);

    syslog(LOG_INFO, "[%s] apn set to %s\n", TAG, apn);
    return 0;
}

/* ── UART / AT ───────────────────────────────────────────────── */

static int cell_uart_open_locked(void)
{
    int fd;
#ifdef CONFIG_SERIAL_TERMIOS
    struct termios tio;
#endif

    if (s_uart_fd >= 0) {
        return s_uart_fd;
    }

    fd = open(CELL_TTY, O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) {
        int err = errno;

        syslog(LOG_ERR, "[%s] open %s failed: %d\n", TAG, CELL_TTY, err);
        return -err;
    }

#ifdef CONFIG_SERIAL_TERMIOS
    /* Air780E 出厂 115200 8N1。父代理会让 UART2 默认 115200，这里再显式设
     * 一次，避免 UART2 被别的用途改成其它波特率后 AT 全变乱码。 */
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        (void)cfsetispeed(&tio, CELL_BAUD);
        (void)cfsetospeed(&tio, CELL_BAUD);
        tio.c_cflag |= (CLOCAL | CREAD);
        /* 板上只引出 PA20/PA27（TX/RX），没有 RTS/CTS：硬件流控必须关，
         * 否则驱动可能等一个永远不来的 CTS。HUPCL 也要关，避免我们
         * 配置完 close 时把模块 hangup 掉。 */
        tio.c_cflag &= ~(CSTOPB | PARENB | CSIZE | CRTSCTS | HUPCL);
        tio.c_cflag |= CS8;
        tio.c_iflag &= ~(IXON | IXOFF | IXANY);
        tio.c_cc[VMIN] = 0;
        tio.c_cc[VTIME] = 0;

        if (tcsetattr(fd, TCSANOW, &tio) < 0) {
            syslog(LOG_WARNING,
                   "[%s] tcsetattr(%s) failed: %d (keep default baud)\n",
                   TAG, CELL_TTY, errno);
        }
    } else {
        syslog(LOG_WARNING, "[%s] tcgetattr(%s) failed: %d\n", TAG, CELL_TTY,
               errno);
    }
#endif

    s_uart_fd = fd;
    return fd;
}

#ifdef CONFIG_NETUTILS_PPPD
static void cell_uart_close_locked(void)
{
    if (s_uart_fd >= 0) {
        close(s_uart_fd);
        s_uart_fd = -1;
    }
}
#endif

static void cell_uart_drain(int fd)
{
    char tmp[64];
    int i;

    for (i = 0; i < 32; i++) {
        ssize_t n = read(fd, tmp, sizeof(tmp));

        if (n > 0) {
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        break;
    }
}

static int cell_write_all(int fd, const char *buf, size_t len)
{
    size_t off = 0;
    uint64_t deadline = cell_now_ms() + CELL_AT_TIMEOUT_MS;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);

        if (n > 0) {
            off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n < 0 && errno != EAGAIN) {
            return -errno;
        }
        if (cell_now_ms() >= deadline) {
            return -ETIMEDOUT;
        }

        {
            struct pollfd pfd;

            pfd.fd = fd;
            pfd.events = POLLOUT;
            pfd.revents = 0;
            (void)poll(&pfd, 1, 50);
        }
    }

    return 0;
}

/* 判断一行是否是 AT 的最终响应。只认整行，避免把
 * "+CGDCONT: ... OK ..." 之类的回显误判成结束。 */
static int cell_rsp_line_final(const char *line, size_t len, int *is_error)
{
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
        len--;
    }
    while (len > 0 && (*line == ' ' || *line == '\t')) {
        line++;
        len--;
    }

    if (len == 2 && memcmp(line, "OK", 2) == 0) {
        return 1;
    }
    if (len == 5 && memcmp(line, "ERROR", 5) == 0) {
        *is_error = 1;
        return 1;
    }
    if (len >= 10 && memcmp(line, "+CME ERROR", 10) == 0) {
        *is_error = 1;
        return 1;
    }
    if (len >= 10 && memcmp(line, "NO CARRIER", 10) == 0) {
        *is_error = 1;
        return 1;
    }
    if (len >= 7 && memcmp(line, "CONNECT", 7) == 0) {
        return 1;
    }

    return 0;
}

static int cell_rsp_done(const char *rsp, int *is_error)
{
    const char *p = rsp;

    while (*p != '\0') {
        const char *eol = p;

        while (*eol != '\0' && *eol != '\r' && *eol != '\n') {
            eol++;
        }

        if (*eol == '\0') {
            break;                       /* 最后半行还没收全 */
        }

        if (cell_rsp_line_final(p, (size_t)(eol - p), is_error)) {
            return 1;
        }

        p = eol;
        while (*p == '\r' || *p == '\n') {
            p++;
        }
    }

    return 0;
}

int cell_at(const char *cmd, char *out, size_t out_cap)
{
    char rsp[CELL_RSP_MAX];
    char tx[192];
    size_t rsp_len = 0;
    uint64_t deadline = 0;
    int tx_len;
    int is_error = 0;
    int fd;
    int rc = 0;
    bool exchanged = false;

    if (out != NULL && out_cap > 0) {
        out[0] = '\0';
    }
    if (cmd == NULL || cmd[0] == '\0') {
        return -EINVAL;
    }

    rsp[0] = '\0';

    pthread_mutex_lock(&s_lock);

#ifdef CONFIG_NETUTILS_PPPD
    if (s_ppp_active || s_ppp_running) {
        pthread_mutex_unlock(&s_lock);
        syslog(LOG_WARNING, "[%s] AT refused: %s is owned by pppd\n", TAG,
               CELL_TTY);
        return -EBUSY;
    }
#endif

    fd = cell_uart_open_locked();
    if (fd < 0) {
        rc = fd;
        goto out;
    }

    cell_uart_drain(fd);

    tx_len = snprintf(tx, sizeof(tx), "%s\r\n", cmd);
    if (tx_len < 0 || (size_t)tx_len >= sizeof(tx)) {
        rc = -EINVAL;
        goto out;
    }

    rc = cell_write_all(fd, tx, (size_t)tx_len);
    if (rc < 0) {
        goto out;
    }

    exchanged = true;
    deadline = cell_now_ms() + CELL_AT_TIMEOUT_MS;

    for (;;) {
        ssize_t nr;
        struct pollfd pfd;
        int pr;

        if (rsp_len >= sizeof(rsp) - 1) {
            rc = -EOVERFLOW;
            break;
        }

        nr = read(fd, rsp + rsp_len, sizeof(rsp) - 1 - rsp_len);
        if (nr > 0) {
            rsp_len += (size_t)nr;
            rsp[rsp_len] = '\0';

            if (cell_rsp_done(rsp, &is_error)) {
                rc = is_error ? -EIO : 0;
                break;
            }
            continue;
        }

        if (nr < 0 && errno != EAGAIN && errno != EINTR) {
            rc = -errno;
            break;
        }

        if (cell_now_ms() >= deadline) {
            rc = -ETIMEDOUT;
            break;
        }

        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, 100);
        if (pr < 0 && errno != EINTR) {
            rc = -errno;
            break;
        }
    }

out:
    if (exchanged) {
        s_uart_ok = (rc == 0) ? 1 : -1;

        if (rsp_len == 0) {
            strncpy(s_last_at, "(no response)", sizeof(s_last_at) - 1);
            s_last_at[sizeof(s_last_at) - 1] = '\0';
        } else {
            size_t keep = rsp_len;

            if (keep > sizeof(s_last_at) - 1) {
                keep = sizeof(s_last_at) - 1;
            }
            memcpy(s_last_at, rsp, keep);
            s_last_at[keep] = '\0';
        }
    }

    if (out != NULL && out_cap > 0) {
        size_t keep = rsp_len;

        if (keep > out_cap - 1) {
            keep = out_cap - 1;
        }
        memcpy(out, rsp, keep);
        out[keep] = '\0';
    }

    if (rc == 0) {
        syslog(LOG_INFO, "[%s] AT: %s -> %.120s\n", TAG, cmd, rsp);
    } else {
        syslog(LOG_WARNING, "[%s] AT: %s -> rc=%d resp=%.120s\n", TAG, cmd, rc,
               rsp);
    }

    pthread_mutex_unlock(&s_lock);
    return rc;
}

int cell_uart_probe(void)
{
    char rsp[64];
    int rc;

    rc = cell_at("AT", rsp, sizeof(rsp));
    if (rc == 0) {
        syslog(LOG_INFO, "[%s] uart probe OK (%s)\n", TAG, CELL_TTY);
    } else {
        syslog(LOG_WARNING, "[%s] uart probe failed: rc=%d\n", TAG, rc);
    }

    return rc;
}

/* ── PPP ─────────────────────────────────────────────────────── */

#ifdef CONFIG_NETUTILS_PPPD

/* 实际送给 chat 的脚本文本（注意：双引号前必须有反斜杠，chat 的
 * tokenizer 才会把引号原样发给模块，而不是把它当成“引用”吃掉）。
 * 例如脚本里是 AT+CGDCONT=1,\"IP\",\"cmnet\" 。 */
#define CELL_PPP_CONNECT_FMT \
    "ECHO ON TIMEOUT 35 \"\" ATE0 " \
    "OK AT+CGDCONT=1,\\\"IP\\\",\\\"%s\\\" " \
    "OK ATD*99***1# CONNECT \\c"

static const char cell_ppp_disconnect_script[] = "\"\" ATZ OK \\c";

static void *cell_ppp_thread(void *arg)
{
    char apn[CELL_APN_MAX];
    char script[256];

    (void)arg;

    s_ppp_running = 1;
    syslog(LOG_INFO, "[%s] ppp thread started on %s\n", TAG, CELL_TTY);

    while (s_ppp_want) {
        struct pppd_settings_s settings;
        int n;
        int ret;
        int i;

        cell_apn_copy(apn, sizeof(apn));

        n = snprintf(script, sizeof(script), CELL_PPP_CONNECT_FMT, apn);
        if (n < 0 || (size_t)n >= sizeof(script)) {
            syslog(LOG_ERR, "[%s] connect script too long (apn=%s)\n", TAG,
                   apn);
            break;
        }

        memset(&settings, 0, sizeof(settings));
        strncpy(settings.ttyname, CELL_TTY, sizeof(settings.ttyname) - 1);
        settings.connect_script = script;
        settings.disconnect_script = cell_ppp_disconnect_script;

        /* stop 可能刚好在 while 判断之后、进 pppd 之前发生；再查一次，
         * 缩小“stop 了却还是把 pppd 拉起来”的窗口。 */
        if (!s_ppp_want) {
            break;
        }

        syslog(LOG_INFO, "[%s] pppd starting (apn=%s)\n", TAG, apn);
        ret = pppd(&settings);
        syslog(LOG_WARNING, "[%s] pppd exited ret=%d want=%d\n", TAG, ret,
               s_ppp_want);

        if (!s_ppp_want) {
            break;
        }

        syslog(LOG_INFO, "[%s] pppd exited, retry in 5s\n", TAG);
        for (i = 0; i < CELL_PPP_RETRY_MS / 100 && s_ppp_want; i++) {
            usleep(100000);
        }
    }

    s_ppp_running = 0;
    s_ppp_active = 0;
    syslog(LOG_INFO, "[%s] ppp thread exited\n", TAG);
    return NULL;
}

int cell_ppp_start(void)
{
    pthread_attr_t attr;
    pthread_t tid;
    int ret;

    cell_net_init();

    pthread_mutex_lock(&s_lock);

    /* 用 active 而不是 running 判重：pthread_create 返回后、线程真正跑起来
     * 之前 running 还是 0，这个窗口里第二次 start 不能再起一个 pppd。 */
    if (s_ppp_active) {
        s_ppp_want = 1;
        pthread_mutex_unlock(&s_lock);
        syslog(LOG_INFO, "[%s] ppp already active\n", TAG);
        return 0;
    }

    /* pppd 自己不设 termios，直接按驱动当前配置发 PPP 帧。这里先 open 一次
     * 把 UART2 配成 115200 8N1 raw，再关掉，保证 pppd 拿到的是二进制通路
     * （canonical/echo 会把 0x0d 之类的 PPP 字节吃掉）。 */
    ret = cell_uart_open_locked();
    if (ret < 0) {
        pthread_mutex_unlock(&s_lock);
        syslog(LOG_ERR, "[%s] ppp start: cannot open %s: %d\n", TAG, CELL_TTY,
               ret);
        return ret;
    }
    cell_uart_close_locked();

#ifndef CONFIG_SERIAL_TERMIOS
    syslog(LOG_WARNING,
           "[%s] CONFIG_SERIAL_TERMIOS=n: cannot force raw 115200 8N1; "
           "pppd may break on line discipline\n", TAG);
#endif

    /* 先占住 tty 再起线程：从这一刻起 cell_at() 会拒绝，避免线程还没
     * 进 pppd 时被一条 AT 插进来。 */
    s_ppp_want = 1;
    s_ppp_active = 1;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, CELL_PPP_STACK);
    ret = pthread_create(&tid, &attr, cell_ppp_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        s_ppp_want = 0;
        s_ppp_active = 0;
        pthread_mutex_unlock(&s_lock);
        syslog(LOG_ERR, "[%s] pthread_create failed: %d\n", TAG, ret);
        return -ret;
    }

    pthread_detach(tid);
    pthread_mutex_unlock(&s_lock);

    syslog(LOG_INFO, "[%s] ppp start requested\n", TAG);
    return 0;
}

int cell_ppp_stop(void)
{
    int was_running;
    int i;

    pthread_mutex_lock(&s_lock);
    s_ppp_want = 0;
    was_running = s_ppp_running;

    /* pppd 用的是它自己 open 的 fd，这里关的是 AT 路径的 fd。关掉可以让
     * 后续 AT 从干净状态重新 open；但打断不了 pppd 的 poll/read。 */
    cell_uart_close_locked();
    pthread_mutex_unlock(&s_lock);

    if (!was_running) {
        syslog(LOG_INFO, "[%s] ppp stop: thread not running\n", TAG);
        return 0;
    }

    for (i = 0; i < 20 && s_ppp_running; i++) {
        usleep(100000);
    }

    if (s_ppp_running) {
        /* 本树 pppd() 是 while(1)，没有 cancel 回调；强行 task_delete 会
         * 泄漏 ctx、tty/tun fd 并使 ppp 网卡悬挂，所以这里只如实报错。 */
        syslog(LOG_WARNING,
               "[%s] pppd is blocked in its own loop and has no cancel hook; "
               "%s stays owned until reboot\n", TAG, CELL_TTY);
        return -EBUSY;
    }

    syslog(LOG_INFO, "[%s] ppp stopped\n", TAG);
    return 0;
}

#else /* !CONFIG_NETUTILS_PPPD */

int cell_ppp_start(void)
{
    syslog(LOG_WARNING, "[%s] pppd not built (CONFIG_NETUTILS_PPPD=n)\n", TAG);
    return -ENOSYS;
}

int cell_ppp_stop(void)
{
    return -ENOSYS;
}

#endif /* CONFIG_NETUTILS_PPPD */

/* ── 状态 ────────────────────────────────────────────────────── */

int cell_get_status(char *out, size_t out_cap)
{
    char apn[CELL_APN_MAX];
    char last[CELL_LAST_RSP_MAX + 1];
    char ppp_state[96];
    char ip[INET_ADDRSTRLEN] = "0.0.0.0";
    struct in_addr addr;
    int uart_fd;
    int uart_ok;
    int have_ip = 0;
#ifdef CONFIG_NETUTILS_PPPD
    int want;
    int running;
    int active;
#endif

    if (out == NULL || out_cap == 0) {
        return -EINVAL;
    }
    out[0] = '\0';

    cell_apn_copy(apn, sizeof(apn));

    pthread_mutex_lock(&s_lock);
    uart_fd = s_uart_fd;
    uart_ok = s_uart_ok;
    strncpy(last, s_last_at, sizeof(last) - 1);
    last[sizeof(last) - 1] = '\0';
#ifdef CONFIG_NETUTILS_PPPD
    want = s_ppp_want;
    running = s_ppp_running;
    active = s_ppp_active;
#endif
    pthread_mutex_unlock(&s_lock);

    /* 响应里的 CR/LF 会把状态排版打散，压成一行 */
    cell_sanitize(last);

#ifdef CONFIG_NETUTILS_PPPD
    snprintf(ppp_state, sizeof(ppp_state), "want=%d running=%d tty=%s", want,
             running, active ? "pppd" : "free");
#else
    strncpy(ppp_state, "disabled (CONFIG_NETUTILS_PPPD=n)",
            sizeof(ppp_state) - 1);
    ppp_state[sizeof(ppp_state) - 1] = '\0';
#endif

    memset(&addr, 0, sizeof(addr));
    if (netlib_get_ipv4addr("ppp0", &addr) == 0 && addr.s_addr != 0) {
        have_ip = 1;
        (void)inet_ntop(AF_INET, &addr, ip, sizeof(ip));
    }

    snprintf(out, out_cap,
             "cell (Air780E, UART2 %s):\n"
             "  apn        : %s\n"
             "  uart       : %s (fd=%d, last AT=%s)\n"
             "  ppp        : %s\n"
             "  ppp0 ipv4  : %s%s\n"
             "  last AT rsp: %s\n",
             CELL_TTY, apn, uart_fd >= 0 ? "open" : "closed", uart_fd,
             uart_ok > 0 ? "ok" : (uart_ok < 0 ? "failed" : "not tried"),
             ppp_state, have_ip ? ip : "none",
             have_ip ? "" : " (ppp0 is created by pppd; name assumed)",
             last[0] != '\0' ? last : "(none)");

    return 0;
}
