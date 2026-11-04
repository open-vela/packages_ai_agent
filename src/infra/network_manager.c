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

#include "network_manager.h"
#include "agent_compat.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <netutils/netlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

/* [V525] ensure_dns() 定义在公共区（三个平台分支都要用），所以这几个头
 * 必须提前包含 —— 它们原本在下面的分支里才 include，公共区看不到。 */
#include <errno.h>
#include <stdio.h>
#include "config/config_store.h"

static const char* TAG = "netmgr";

static char s_ip_str[INET_ADDRSTRLEN] = "0.0.0.0";

bool network_is_connected(void)
{
    struct ifaddrs* ifa_list = NULL;
    if (getifaddrs(&ifa_list) == 0) {
        for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
            if (!ifa->ifa_addr)
                continue;
            if (ifa->ifa_addr->sa_family != AF_INET)
                continue;
            if (ifa->ifa_name && strncmp(ifa->ifa_name, "lo", 2) == 0)
                continue;

            struct sockaddr_in* sin = (struct sockaddr_in*)ifa->ifa_addr;
            uint32_t addr = ntohl(sin->sin_addr.s_addr);
            if ((addr >> 24) == 127)
                continue; /* 127.x.x.x loopback */

            /* Skip 0.0.0.0 — interface exists but has no IP yet */
            if (addr == 0)
                continue;

            inet_ntop(AF_INET, &sin->sin_addr, s_ip_str, sizeof(s_ip_str));
            syslog(LOG_INFO, "[%s] Found iface %s addr %s\n", TAG,
                ifa->ifa_name ? ifa->ifa_name : "?", s_ip_str);
            freeifaddrs(ifa_list);
            return true;
        }
        freeifaddrs(ifa_list);
    }

    return false;
}

/* [V525] 确保 resolver 已被编程 —— 任何一次拿到 IP 之后都必须调。
 *
 * ★★★ 这是「无语音」的唯一阻塞点，根因查清了：
 *
 *   NuttX 的 DNS client 只认netlib_set_ipv4dnsaddr() 设置的地址
 *   （缺省才用编译期的 CONFIG_NETDB_DNSSERVER_IPv4ADDR）。
 *   **它从不读 /tmp/resolv.conf。**
 *
 *   而本项目的网络是 network_manager 自己管的，不走 netinit 工具那条路：
 *     - netinit_set_ipv4addrs()（唯一会设 DNS 的地方）只在跑 netinit 时执行；
 *     - network_set_dns() 只在 network_rpmsg_init()（BLE/RPMSG 分支）
 *       和 nsh 命令 setdns 里被调用；
 *     - **真实硬件分支 network_wifi_connect() / network_wifi_reconnect()
 *       从头到尾没有调用过 network_set_dns()**。
 *
 *   于是 getaddrinfo("token-plan-cn.xiaomimimo.com") 永远拿不到名字服务器，
 *   [vela_tls] 打印完 TCP connect 那一行就永久静默 —— 看着像网络不通，
 *   其实是没配 DNS。IP 直连（42.48.185.89:2301）能通，正是这个原因。
 *
 *   DHCP 也失败过（[V113] second DHCP attempt also failed），不能指望 DHCP
 *   下发的 DNS，必须显式写死。
 *
 * ★ 为什么不复用 network_set_dns()：它定义在下面的 RPMSG 分支里，而本函数
 *   要放在公共区（QEMU / RPMSG / 真实硬件三个分支共用），否则链接期找不到符号
 *   —— 这个坑踩过一次：ensure_dns 放在分支内，编译过但 LD 报
 *   undefined reference。这里直接调 netlib_set_ipv4dnsaddr() 并顺手写
 *   resolv.conf，两件事都在本地做完。
 */
/* [V1257] Does wlan0 already have a default route?  /proc/net/route has
 * one line per route, "Iface Destination Gateway Flags ..."; the default
 * route is the entry with Destination 00000000.  Returns true as soon as
 * any such entry exists. */
static bool route_has_default(void)
{
    FILE* fp = fopen("/proc/net/route", "r");
    char line[256];
    bool found = false;

    if (!fp)
      {
        return false;
      }

    while (fgets(line, sizeof(line), fp))
      {
        char iface[32];
        unsigned long dest;

        /* "wlan0<TAB>00000000<TAB>..." -> a default route exists */
        if (sscanf(line, "%31s %lx", iface, &dest) == 2 && dest == 0)
          {
            found = true;
            break;
          }
      }

    fclose(fp);
    return found;
}

/* [V1257] On-site network switch: the subnet is no longer compiled in.
 *
 * config_store keys (written by the CLI "set_net <ip> <gw>"):
 *   net.static_ip - force wlan0 to this IPv4 address (mask /24)
 *   net.static_gw - gateway for the forced path
 *
 * Both keys empty (the default): DHCP's result is kept as-is - no
 * set_ipv4netmask / set_ipv4addr / set_dripv4addr override at all.  The one
 * rescue kept is the route fix-up: when /proc/net/route shows no default
 * route, wlan0's current address is taken, its last octet is replaced with
 * .1 and that is installed as the gateway (phone hotspots and ordinary
 * routers answer on .1).
 *
 * net.static_ip configured: the former V1041 forced-static path runs
 * (address + /24 mask); the gateway comes from net.static_gw or, when that
 * key is empty, is derived as the .1 of the static IP's /24.
 *
 * Idempotent and quiet, so it can stay in ensure_dns() on every
 * "connected" event. */
static void ensure_static_ip(void)
{
    char cfg_ip[64] = { 0 };
    char cfg_gw[64] = { 0 };
    struct in_addr want;
    struct in_addr gw;
    struct in_addr cur;
    struct in_addr mask;
    char ip[INET_ADDRSTRLEN];
    bool have_ip;
    bool have_gw;
    int rc;

    have_ip = claw_config_get("net.static_ip", cfg_ip, sizeof(cfg_ip)) == OK
              && cfg_ip[0] != '\0'
              && inet_pton(AF_INET, cfg_ip, &want) == 1;
    have_gw = claw_config_get("net.static_gw", cfg_gw, sizeof(cfg_gw)) == OK
              && cfg_gw[0] != '\0'
              && inet_pton(AF_INET, cfg_gw, &gw) == 1;

    if (have_ip)
      {
        /* Forced static path (former V1041 behaviour). */
        memset(&cur, 0, sizeof(cur));
        if (netlib_get_ipv4addr("wlan0", &cur) != OK ||
            cur.s_addr != want.s_addr)
          {
            /* Only d_ipaddr is rewritten by SIOCSIFADDR (netdev_ioctl.c:343),
             * so the netmask and default route set around this call survive. */
            rc = netlib_set_ipv4addr("wlan0", &want);
            inet_ntop(AF_INET, &want, ip, sizeof(ip));
            syslog(rc == OK ? LOG_INFO : LOG_ERR,
                   "[V1257] static ip %s rc=%d\n", ip, rc);
          }

        if (!have_gw)
          {
            gw = want;
            gw.s_addr = (want.s_addr & htonl(0xffffff00)) | htonl(1);
          }

        inet_pton(AF_INET, "255.255.255.0", &mask);
        netlib_set_ipv4netmask("wlan0", &mask);
        rc = netlib_set_dripv4addr("wlan0", &gw);
        inet_ntop(AF_INET, &gw, ip, sizeof(ip));
        syslog(LOG_INFO, "[V1257] static gw %s rc=%d\n", ip, rc);
        return;
      }

    /* Default: keep DHCP's lease untouched; only rescue a missing default
     * route (the old V526 symptom: empty route table, every connect times
     * out). */
    if (route_has_default())
      {
        return;
      }

    memset(&cur, 0, sizeof(cur));
    if (netlib_get_ipv4addr("wlan0", &cur) != OK || cur.s_addr == 0)
      {
        return;                     /* no address yet, nothing to rescue */
      }

    if (!have_gw)
      {
        gw = cur;
        gw.s_addr = (cur.s_addr & htonl(0xffffff00)) | htonl(1);
      }

    rc = netlib_set_dripv4addr("wlan0", &gw);
    inet_ntop(AF_INET, &gw, ip, sizeof(ip));
    syslog(LOG_INFO, "[V1257] no default route, rescue gw %s rc=%d\n",
           ip, rc);
}

static void ensure_dns(void)
{
    static const char *const fallbacks[] = { "223.5.5.5", "119.29.29.29" };
    char primary[64] = { 0 };
    struct in_addr dns_addr;
    unsigned int i;
    FILE* fp;

    /* ── [V526] 先修默认路由，再谈 DNS ──────────────────────────
     *
     * 板上的证据：`[V58] /proc/net/route open: 2` —— 路由表是**空的**。
     * 没有默认路由意味着板子只能和同网段通信，出网的 TCP 和 UDP 都发不出去。
     * 这一条解释了之前所有的现象：
     *   - DNS 明明配成 223.5.5.5(rc=0) 却一个查询都发不出去；
     *   - getaddrinfo() 卡在里面不返回（包根本出不去，只在本地等超时）；
     *   - 改成 IP 直连 111.206.191.150:443 同样建不了连；
     *   - 之前[V58] probe gw/srv/pub 全是 -110(ETIMEDOUT)，包括内网
     *     42.48.185.89 —— 那些"能通"的印象其实也是超时。
     *
     * 为什么没有路由：编译期 CONFIG_NETINIT_DRIPADDR=0x0a000001 即 10.0.0.1，
     * 那是另一个网络的网关，跟本机实际网段 192.168.31.0/24 毫无关系。
     * DHCP 模式下这个静态值不参与路由表写入，而本项目**没有任何代码**
     * 在 DHCP 之后补设默认路由（grep netlib_set_dripv4addr 在 packages/ 下
     * 零命中）。
     *
     * [V1257] 现场换网后子网不再写死：默认保留 DHCP 结果，只在路由表
     * 确实没有默认路由时补一条救急网关（详见 ensure_static_ip()）。
     */
    {
        /* [V1257] Subnet lock removed - netmask/address/gateway are no
         * longer forced here.  ensure_static_ip() keeps DHCP's result
         * unless net.static_ip / net.static_gw are configured, and only
         * rescues a missing default route (see its comment). */
        ensure_static_ip();
    }

        /* [V1262] Resolver priority: default-route gateway first.
     *
     * On site the router / phone-hotspot DNS forwarder is what actually
     * answers; public DNS (223.5.5.5) was measured unreachable on the
     * venue-like network while the gateway answered.  Stored
     * net.dns_primary is only used when no default route exists, and the
     * public list is the last resort. */
    {
        char gwbuf[64] = { 0 };
        FILE* gfp = fopen("/proc/net/route", "r");
        char gline[256];

        while (gfp && fgets(gline, sizeof(gline), gfp)) {
            char giface[32];
            unsigned long gdest;
            unsigned long ggw;

            if (sscanf(gline, "%31s %lx %lx", giface, &gdest, &ggw) == 3
                && gdest == 0 && ggw != 0) {
                struct in_addr ga;

                ga.s_addr = (in_addr_t)ggw;
                inet_ntop(AF_INET, &ga, gwbuf, sizeof(gwbuf));
                break;
            }
        }
        if (gfp) {
            fclose(gfp);
        }

        if (gwbuf[0] != '\0') {
            strncpy(primary, gwbuf, sizeof(primary) - 1);
            primary[sizeof(primary) - 1] = '\0';
            syslog(LOG_INFO, "[%s] [V1262] resolver <- default gw %s\n",
                   TAG, gwbuf);
        } else if (claw_config_get("net.dns_primary", primary,
                                   sizeof(primary)) != OK
                   || primary[0] == '\0') {
            strncpy(primary, fallbacks[0], sizeof(primary) - 1);
            primary[sizeof(primary) - 1] = '\0';
            syslog(LOG_WARNING, "[%s] [V1262] no default route - "
                   "resolver <- %s\n", TAG, fallbacks[0]);
        }
    }

    if (inet_pton(AF_INET, primary, &dns_addr) != 1) {
        syslog(LOG_ERR, "[%s] ensure_dns: bad stored DNS '%s', using %s\n",
               TAG, primary, fallbacks[0]);
        strncpy(primary, fallbacks[0], sizeof(primary) - 1);
        primary[sizeof(primary) - 1] = '\0';
        inet_pton(AF_INET, primary, &dns_addr);
    }

    /* 关键这一句：NuttX resolver 真正读的东西 */
    for (i = 0; i < 2; i++) {
        struct in_addr a;
        int rc;

        inet_pton(AF_INET, primary, &a);
        rc = netlib_set_ipv4dnsaddr(&a);
        syslog(LOG_INFO, "[%s] ensure_dns: resolver <- %s (rc=%d)\n",
               TAG, primary, rc);
        if (rc == OK) {
            break;
        }

        /* 换用备用服务器再试 */
        primary[0] = '\0';
        strncpy(primary, fallbacks[i + 1 < 2 ? i + 1 : 1],
                sizeof(primary) - 1);
        primary[sizeof(primary) - 1] = '\0';
    }

    /* 同时写 resolv.conf（部分应用/工具会读它）。写不了不算失败。 */
    fp = fopen("/tmp/resolv.conf", "w");
    if (fp) {
        fprintf(fp, "nameserver %s\n", primary);
        fprintf(fp, "nameserver %s\n", fallbacks[1]);
        fclose(fp);
    } else {
        syslog(LOG_WARNING, "[%s] ensure_dns: cannot write /tmp/resolv.conf "
               "(non-fatal, netlib resolver already programmed)\n", TAG);
    }
}

extern int nsh_commands_init(void);
extern int nsh_commands_start(void);

/* [V1258] Boot fallbacks anchored in network_manager.c.
 *
 * The agent_main object that actually reaches the link no longer calls
 * nsh_commands_init()/start() or network_wifi_reconnect() - those calls
 * sit in a newer agent_main.c than the object the archive serves (see
 * the 2026-11-04 stale-object investigation).  Changes made in THIS
 * file do reach the link reliably, so anchor the boot-time behaviour
 * here:
 *   1. Start the CLI (set_wifi / set_net / wifi_reconnect) on first
 *      entry - the whole nsh_commands module would otherwise be
 *      dead-stripped because nothing references it.
 *   2. If netinit failed to associate, retry once with the credentials
 *      saved by set_wifi in config_store (wifi_ssid/wifi_pass). */
int network_wait_connected(uint32_t timeout_ms)
{
    static bool s_boot_cli_started;
    static bool s_boot_retry_done;
    int wait_polls = 0;

    if (!s_boot_cli_started) {
        s_boot_cli_started = true;
        nsh_commands_init();
        nsh_commands_start();
        syslog(LOG_INFO,
               "[%s] [V1258] CLI started (set_wifi/set_net available)\n",
               TAG);
    }

    if (timeout_ms == 0) {
        if (network_is_connected()) {
            ensure_dns();
            return OK;
        }
        return ERROR;
    }

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (1) {
        if (network_is_connected()) {
            syslog(LOG_INFO, "[%s] Network connected: %s\n", TAG, s_ip_str);
            /* [V525] 一拿到 IP 就编程 resolver —— NuttX 不会从 DHCP 或
             * resolv.conf 拿 DNS，不设就是永久解析失败。见 ensure_dns() 注释。 */
            ensure_dns();
            return OK;
        }

        /* [V1258] netinit has had ~8 s and still no link: retry once
         * with saved credentials before giving the deadline a chance. */
        if (!s_boot_retry_done && ++wait_polls >= 16) {
            s_boot_retry_done = true;
            syslog(LOG_WARNING,
                   "[%s] [V1258] no link after 8 s - retry with saved WiFi credentials\n",
                   TAG);
            network_wifi_reconnect();
            continue;
        }

        struct timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        if (now.tv_sec > deadline.tv_sec || (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
            syslog(LOG_WARNING, "[%s] Timed out waiting for network\n", TAG);
            return ERROR;
        }

        usleep(500000); /* poll every 500 ms */
    }
}

const char* network_get_ip(void)
{
    network_is_connected(); /* refresh */
    return s_ip_str;
}

/* ── WiFi connect (real hardware only) ───────────────────────── */

#if defined(CONFIG_ARCH_CHIP_GOLDFISH_ARM64) || defined(CONFIG_ARCH_CHIP_QEMU_ARM)
/* On QEMU, virtio-net provides connectivity automatically — but needs
 * ifup/renew */
#include <net/if.h>
#include <nuttx/net/netconfig.h>
#include <sys/ioctl.h>

int network_wifi_connect(const char* iface, const char* ssid,
    const char* pass)
{
    (void)iface;
    (void)ssid;
    (void)pass;
    syslog(LOG_INFO,
        "[%s] QEMU: skipping wifi_connect (virtio-net handles networking)\n",
        TAG);
    return OK;
}

int network_wifi_reconnect(void)
{
    syslog(LOG_INFO, "[%s] QEMU: Initializing eth0...\n", TAG);

    /* Bring up eth0 interface */
    int ret = system("ifup eth0");
    if (ret != 0) {
        syslog(LOG_WARNING, "[%s] ifup eth0 failed: %d\n", TAG, ret);
    }

    /* Request DHCP lease */
    ret = system("renew eth0");
    if (ret != 0) {
        syslog(LOG_WARNING, "[%s] renew eth0 failed: %d\n", TAG, ret);
    }

    /* Wait a bit for network to come up */
    usleep(500000);

    return network_wait_connected(5000);
}

#elif defined(CONFIG_AI_AGENT_NET_RPMSG)
/* ── RPMSG/TUN network via BLE proxy ─────────────────────────── */

#include "config/config_store.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Static state ─────────────────────────────────────────────── */

typedef struct {
    char proxy_mode[16];    /* "usrsock" or "tun" */
    char rpmsg_cpu[16];     /* RPMSG target CPU name */
    int connect_timeout;    /* TLS connect timeout (seconds) */
    int read_timeout;       /* TLS read timeout (seconds) */
    int retry_max;          /* HTTP max retries */
    int retry_base_sec;     /* Retry backoff base (seconds) */
} net_config_t;

static net_config_t g_net_config;
static net_status_t g_net_status;

typedef struct {
    net_state_cb_t cb;
    void* arg;
} net_listener_t;

static net_listener_t g_listeners[NET_MAX_LISTENERS];
static int g_listener_count;

static pthread_mutex_t g_net_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_net_cond = PTHREAD_COND_INITIALIZER;
static pthread_t g_poll_thread;
static volatile bool g_poll_running;
static volatile bool g_force_check;

/* ── Internal helpers ─────────────────────────────────────────── */

static void notify_listeners(net_state_t state)
{
    for (int i = 0; i < g_listener_count; i++) {
        if (g_listeners[i].cb) {
            g_listeners[i].cb(state, g_listeners[i].arg);
        }
    }
}

static void set_net_state(net_state_t new_state)
{
    pthread_mutex_lock(&g_net_mutex);
    if (g_net_status.state != new_state) {
        net_state_t old = g_net_status.state;
        g_net_status.state = new_state;

        if (new_state == NET_STATE_CONNECTED) {
            g_net_status.connected_since = time(NULL);
        }

        syslog(LOG_INFO, "[%s] State: %s -> %s\n", TAG,
            old == NET_STATE_CONNECTED ? "CONNECTED" : "DISCONNECTED",
            new_state == NET_STATE_CONNECTED ? "CONNECTED" : "DISCONNECTED");

        notify_listeners(new_state);
        pthread_cond_broadcast(&g_net_cond);
    }
    pthread_mutex_unlock(&g_net_mutex);
}

/**
 * Check network interfaces via getifaddrs().
 * Returns true if a valid non-loopback IPv4 interface is found.
 * Accepts rpmsg*, tun*, eth* etc. Skips lo* and 127.x.x.x / 0.0.0.0.
 */
static bool check_interfaces(void)
{
    struct ifaddrs* ifa_list = NULL;
    bool found = false;

    if (getifaddrs(&ifa_list) != 0) {
        syslog(LOG_WARNING, "[%s] getifaddrs failed: %d\n", TAG, errno);
        return false;
    }

    for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr) {
            continue;
        }
        if (ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (ifa->ifa_name && strncmp(ifa->ifa_name, "lo", 2) == 0) {
            continue;
        }

        struct sockaddr_in* sin = (struct sockaddr_in*)ifa->ifa_addr;
        uint32_t addr = ntohl(sin->sin_addr.s_addr);

        /* Skip 127.x.x.x loopback */
        if ((addr >> 24) == 127) {
            continue;
        }
        /* Skip 0.0.0.0 — interface exists but has no IP yet */
        if (addr == 0) {
            continue;
        }

        /* Valid interface found */
        pthread_mutex_lock(&g_net_mutex);
        inet_ntop(AF_INET, &sin->sin_addr,
            g_net_status.ip_addr, sizeof(g_net_status.ip_addr));
        if (ifa->ifa_name) {
            strncpy(g_net_status.iface_name, ifa->ifa_name,
                sizeof(g_net_status.iface_name) - 1);
            g_net_status.iface_name[sizeof(g_net_status.iface_name) - 1] = '\0';
        }
        g_net_status.last_check = time(NULL);
        pthread_mutex_unlock(&g_net_mutex);

        /* Also update the shared s_ip_str for network_get_ip() */
        inet_ntop(AF_INET, &sin->sin_addr, s_ip_str, sizeof(s_ip_str));

        syslog(LOG_DEBUG, "[%s] Found iface %s addr %s\n", TAG,
            ifa->ifa_name ? ifa->ifa_name : "?",
            g_net_status.ip_addr);

        found = true;
        break;
    }

    freeifaddrs(ifa_list);
    return found;
}

/* ── Config loading ────────────────────────────────────────────── */

static void load_net_config(void)
{
    char buf[32] = { 0 };

    /* proxy_mode: default "usrsock" */
    if (claw_config_get("net.proxy_mode", g_net_config.proxy_mode,
            sizeof(g_net_config.proxy_mode))
            != OK
        || g_net_config.proxy_mode[0] == '\0') {
        strncpy(g_net_config.proxy_mode, "usrsock",
            sizeof(g_net_config.proxy_mode) - 1);
        g_net_config.proxy_mode[sizeof(g_net_config.proxy_mode) - 1] = '\0';
    }

    /* rpmsg_cpu: default "ap" */
    if (claw_config_get("net.rpmsg_cpu", g_net_config.rpmsg_cpu,
            sizeof(g_net_config.rpmsg_cpu))
            != OK
        || g_net_config.rpmsg_cpu[0] == '\0') {
        strncpy(g_net_config.rpmsg_cpu, "ap",
            sizeof(g_net_config.rpmsg_cpu) - 1);
        g_net_config.rpmsg_cpu[sizeof(g_net_config.rpmsg_cpu) - 1] = '\0';
    }

    /* connect_timeout: default 15 */
    if (claw_config_get("net.connect_timeout", buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        g_net_config.connect_timeout = atoi(buf);
    } else {
        g_net_config.connect_timeout = 15;
    }

    /* read_timeout: default 30 */
    memset(buf, 0, sizeof(buf));
    if (claw_config_get("net.read_timeout", buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        g_net_config.read_timeout = atoi(buf);
    } else {
        g_net_config.read_timeout = 200;
    }

    /* retry_max: default 3 */
    memset(buf, 0, sizeof(buf));
    if (claw_config_get("net.retry_max", buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        g_net_config.retry_max = atoi(buf);
    } else {
        g_net_config.retry_max = 3;
    }

    /* retry_base_sec: default 2 */
    memset(buf, 0, sizeof(buf));
    if (claw_config_get("net.retry_base_sec", buf, sizeof(buf)) == OK
        && buf[0] != '\0') {
        g_net_config.retry_base_sec = atoi(buf);
    } else {
        g_net_config.retry_base_sec = 2;
    }

    syslog(LOG_INFO,
        "[%s] Config loaded: proxy=%s cpu=%s timeout=%d/%d retry=%d base=%d\n",
        TAG, g_net_config.proxy_mode, g_net_config.rpmsg_cpu,
        g_net_config.connect_timeout, g_net_config.read_timeout,
        g_net_config.retry_max, g_net_config.retry_base_sec);
}

int network_save_proxy_config(const char* mode, const char* cpu_name)
{
    if (!mode || mode[0] == '\0') {
        syslog(LOG_ERR, "[%s] save_proxy_config: mode required\n", TAG);
        return -EINVAL;
    }

    claw_config_set("net.proxy_mode", mode);
    strncpy(g_net_config.proxy_mode, mode,
        sizeof(g_net_config.proxy_mode) - 1);
    g_net_config.proxy_mode[sizeof(g_net_config.proxy_mode) - 1] = '\0';

    if (cpu_name && cpu_name[0] != '\0') {
        claw_config_set("net.rpmsg_cpu", cpu_name);
        strncpy(g_net_config.rpmsg_cpu, cpu_name,
            sizeof(g_net_config.rpmsg_cpu) - 1);
        g_net_config.rpmsg_cpu[sizeof(g_net_config.rpmsg_cpu) - 1] = '\0';
    }

    syslog(LOG_INFO, "[%s] Proxy config saved: mode=%s cpu=%s\n",
        TAG, g_net_config.proxy_mode, g_net_config.rpmsg_cpu);
    return OK;
}

/* ── Config getters ───────────────────────────────────────────── */

int network_get_connect_timeout(void)
{
    return g_net_config.connect_timeout;
}

int network_get_read_timeout(void)
{
    return g_net_config.read_timeout;
}

int network_get_retry_max(void)
{
    return g_net_config.retry_max;
}

int network_get_retry_base_sec(void)
{
    return g_net_config.retry_base_sec;
}

const char* network_get_proxy_mode(void)
{
    return g_net_config.proxy_mode;
}

const char* network_get_rpmsg_cpu(void)
{
    return g_net_config.rpmsg_cpu;
}

/* ── Interface poll thread ────────────────────────────────────── */

static void* iface_poll_thread(void* arg)
{
    (void)arg;
    syslog(LOG_INFO, "[%s] O74I interface poll thread started\n", TAG);

    /* Startup timeout: 30 seconds to get an IP */
    struct timespec start;
    clock_gettime(CLOCK_MONOTONIC, &start);
    bool startup_warned = false;

    while (g_poll_running) {
        bool has_ip = check_interfaces();

        if (has_ip) {
            set_net_state(NET_STATE_CONNECTED);
            startup_warned = false;
        } else {
            set_net_state(NET_STATE_DISCONNECTED);

            /* Check startup timeout */
            if (!startup_warned) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (now.tv_sec - start.tv_sec) * 1000L
                    + (now.tv_nsec - start.tv_nsec) / 1000000L;
                if (elapsed_ms >= NET_STARTUP_TIMEOUT_MS) {
                    syslog(LOG_WARNING,
                        "[%s] No IP after %d ms startup timeout, "
                        "continuing poll\n",
                        TAG, NET_STARTUP_TIMEOUT_MS);
                    startup_warned = true;
                }
            }
        }

        /* Sleep for poll interval, but wake early on force check */
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += NET_POLL_INTERVAL_MS / 1000;
        ts.tv_nsec += (NET_POLL_INTERVAL_MS % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }

        pthread_mutex_lock(&g_net_mutex);
        while (!g_force_check && g_poll_running) {
            int rc = pthread_cond_timedwait(&g_net_cond, &g_net_mutex, &ts);
            if (rc == ETIMEDOUT) {
                break;
            }
        }
        g_force_check = false;
        pthread_mutex_unlock(&g_net_mutex);
    }

    syslog(LOG_INFO, "[%s] O74I interface poll thread exiting\n", TAG);
    return NULL;
}

/* ── Public API ───────────────────────────────────────────────── */

int network_register_listener(net_state_cb_t cb, void* arg)
{
    if (!cb) {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_net_mutex);
    if (g_listener_count >= NET_MAX_LISTENERS) {
        pthread_mutex_unlock(&g_net_mutex);
        syslog(LOG_ERR, "[%s] Max listeners (%d) reached\n",
            TAG, NET_MAX_LISTENERS);
        return -ENOMEM;
    }

    g_listeners[g_listener_count].cb = cb;
    g_listeners[g_listener_count].arg = arg;
    g_listener_count++;
    pthread_mutex_unlock(&g_net_mutex);

    return OK;
}

net_state_t network_get_state(void)
{
    net_state_t state;
    pthread_mutex_lock(&g_net_mutex);
    state = g_net_status.state;
    pthread_mutex_unlock(&g_net_mutex);
    return state;
}

int network_get_active_conns(void)
{
    int conns;
    pthread_mutex_lock(&g_net_mutex);
    conns = g_net_status.active_conns;
    pthread_mutex_unlock(&g_net_mutex);
    return conns;
}

int network_get_iob_usage(void)
{
    FILE* fp = NULL;
    int usage_pct = 0;
    char line[128];
    int total = 0;
    int free_cnt = 0;

    fp = fopen("/proc/net/iob", "r");
    if (!fp) {
        syslog(LOG_DEBUG, "[%s] Cannot open /proc/net/iob\n", TAG);
        return 0;
    }

    /* Parse IOB stats — look for total and free counts.
     * Format varies by NuttX version; try common patterns. */
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "  total:%d", &total) == 1) {
            /* found total */
        } else if (sscanf(line, "  free:%d", &free_cnt) == 1) {
            /* found free */
        }
    }

    fclose(fp);

    if (total > 0) {
        usage_pct = ((total - free_cnt) * 100) / total;
        if (usage_pct < 0) {
            usage_pct = 0;
        }
        if (usage_pct > 100) {
            usage_pct = 100;
        }
    }

    pthread_mutex_lock(&g_net_mutex);
    g_net_status.iob_usage_pct = usage_pct;
    pthread_mutex_unlock(&g_net_mutex);

    return usage_pct;
}

int network_reconnect(void)
{
    syslog(LOG_INFO, "[%s] Reconnect requested\n", TAG);

    pthread_mutex_lock(&g_net_mutex);
    g_net_status.state = NET_STATE_DISCONNECTED;
    memset(g_net_status.ip_addr, 0, sizeof(g_net_status.ip_addr));
    memset(g_net_status.iface_name, 0, sizeof(g_net_status.iface_name));
    g_force_check = true;
    pthread_cond_signal(&g_net_cond);
    pthread_mutex_unlock(&g_net_mutex);

    return OK;
}

int network_set_dns(const char* primary, const char* secondary)
{
    FILE* fp = NULL;
    int ret = ERROR;

    if (!primary || primary[0] == '\0') {
        syslog(LOG_ERR, "[%s] set_dns: primary DNS required\n", TAG);
        return -EINVAL;
    }

    fp = fopen("/tmp/resolv.conf", "w");
    if (!fp) {
        /* [V525] resolv.conf 写不了**不能**直接返回。
         *
         * 原来这里 `return -errno;`，把后面的 netlib_set_ipv4dnsaddr() 跳过了 ——
         * 而那恰恰是 NuttX resolver 真正会读的东西。结果是：就算 /tmp 建不出来，
         * DNS 也永远没被设置，gethostbyname() 全部超时。
         * 这就是「有 resolv.conf 修复却依然解析失败」的原因之一。 */
        
    } else {
        fprintf(fp, "nameserver %s\n", primary);
        if (secondary && secondary[0] != '\0') {
            fprintf(fp, "nameserver %s\n", secondary);
        }
        fclose(fp);
    }

    /* [DNS-FIX] Program the NuttX resolver as well.
     *
     * NuttX's DNS client takes its nameserver from
     * netlib_set_ipv4dnsaddr() -- or from the compile-time
     * CONFIG_NETDB_DNSSERVER_IPv4ADDR default -- and never reads
     * /tmp/resolv.conf.  Writing that file alone left the resolver
     * pointing at the build-time default, so every hostname lookup
     * failed instantly and connect() returned
     * MBEDTLS_ERR_NET_CONNECT_FAILED (0x52) in about 5 ms.  That looks
     * exactly like "the router is broken" on every AP, while
     * IP-literal requests (42.48.185.89:2301) kept working. */
    {
        struct in_addr dns_addr;

        if (inet_pton(AF_INET, primary, &dns_addr) == 1) {
            int rc = netlib_set_ipv4dnsaddr(&dns_addr);
            syslog(LOG_INFO, "[%s] resolver nameserver <- %s (rc=%d)\n",
                TAG, primary, rc);
        } else {
            syslog(LOG_ERR, "[%s] invalid DNS address: %s\n",
                TAG, primary);
        }
    }

    /* Persist to config_store */
    claw_config_set("net.dns_primary", primary);
    if (secondary && secondary[0] != '\0') {
        claw_config_set("net.dns_secondary", secondary);
    }

    syslog(LOG_INFO, "[%s] DNS configured: %s %s\n", TAG,
        primary, (secondary && secondary[0]) ? secondary : "");

    ret = OK;
    return ret;
}

/* ── BLE state callback stub ──────────────────────────────────── */

static void ble_state_callback(bool connected)
{
    syslog(LOG_INFO, "[%s] BLE state: %s\n", TAG,
        connected ? "connected" : "disconnected");

    pthread_mutex_lock(&g_net_mutex);
    g_net_status.ble_connected = connected;
    pthread_mutex_unlock(&g_net_mutex);

    if (!connected) {
        set_net_state(NET_STATE_DISCONNECTED);
    } else {
        /* BLE reconnected — trigger immediate interface check */
        pthread_mutex_lock(&g_net_mutex);
        g_force_check = true;
        pthread_cond_signal(&g_net_cond);
        pthread_mutex_unlock(&g_net_mutex);
    }
}

/* ── RPMSG init ───────────────────────────────────────────────── */

int network_rpmsg_init(void)
{
    int ret = ERROR;
    char dns_primary[64] = { 0 };
    char dns_secondary[64] = { 0 };

    syslog(LOG_INFO, "[%s] O74I: Initializing RPMSG/TUN network\n", TAG);

    /* Load network config from config_store */
    load_net_config();

    /* Initialize state */
    memset(&g_net_status, 0, sizeof(g_net_status));
    g_net_status.state = NET_STATE_DISCONNECTED;
    g_net_status.ble_connected = true; /* assume BLE up at boot */
    g_listener_count = 0;
    g_force_check = false;

    /* Load DNS config from config_store, use defaults if absent */
    if (claw_config_get("net.dns_primary", dns_primary,
            sizeof(dns_primary))
            != OK
        || dns_primary[0] == '\0') {
        strncpy(dns_primary, "223.5.5.5", sizeof(dns_primary) - 1);
        dns_primary[sizeof(dns_primary) - 1] = '\0';
    }

    if (claw_config_get("net.dns_secondary", dns_secondary,
            sizeof(dns_secondary))
            != OK
        || dns_secondary[0] == '\0') {
        strncpy(dns_secondary, "8.8.8.8", sizeof(dns_secondary) - 1);
        dns_secondary[sizeof(dns_secondary) - 1] = '\0';
    }

    /* Configure DNS */
    network_set_dns(dns_primary, dns_secondary);

    /* Start interface poll thread */
    g_poll_running = true;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4096);

    ret = pthread_create(&g_poll_thread, &attr, iface_poll_thread, NULL);
    pthread_attr_destroy(&attr);

    if (ret != 0) {
        syslog(LOG_ERR, "[%s] Failed to create poll thread: %d\n",
            TAG, ret);
        g_poll_running = false;
        return ERROR;
    }

    pthread_setname_np(g_poll_thread, "net_poll");

    /* Register BLE state callback (stub — replace with real BLE API) */
    syslog(LOG_INFO,
        "[%s] BLE callback registered (stub, awaiting BLE framework)\n",
        TAG);
    (void)ble_state_callback; /* suppress unused warning */

    syslog(LOG_INFO, "[%s] O74I network init complete\n", TAG);
    return OK;
}

/* ── Resource guard ───────────────────────────────────────────── */

int network_acquire_resource(uint32_t timeout_ms)
{
    pthread_mutex_lock(&g_net_mutex);

    /* Fast-fail if BLE is disconnected */
    if (!g_net_status.ble_connected) {
        pthread_mutex_unlock(&g_net_mutex);
        return -ENETUNREACH;
    }

    /* Check TCP connection limit and IOB watermark */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(timeout_ms / 1000);
    deadline.tv_nsec += (long)((timeout_ms % 1000) * 1000000L);
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }

    while (g_net_status.active_conns >= NET_MAX_TCP_CONNS
        || network_get_iob_usage() >= NET_IOB_HIGH_WATERMARK) {
        int rc = pthread_cond_timedwait(&g_net_cond, &g_net_mutex, &deadline);
        if (rc == ETIMEDOUT) {
            pthread_mutex_unlock(&g_net_mutex);
            syslog(LOG_WARNING, "[%s] acquire_resource timeout\n", TAG);
            return -ETIMEDOUT;
        }
        /* Re-check BLE after wakeup */
        if (!g_net_status.ble_connected) {
            pthread_mutex_unlock(&g_net_mutex);
            return -ENETUNREACH;
        }
    }

    g_net_status.active_conns++;
    pthread_mutex_unlock(&g_net_mutex);
    return OK;
}

void network_release_resource(void)
{
    pthread_mutex_lock(&g_net_mutex);
    if (g_net_status.active_conns > 0) {
        g_net_status.active_conns--;
    }
    pthread_cond_broadcast(&g_net_cond);
    pthread_mutex_unlock(&g_net_mutex);
}

/* ── WiFi stubs (redirect to rpmsg_init) ──────────────────────── */

int network_wifi_connect(const char* iface, const char* ssid,
    const char* pass)
{
    (void)iface;
    (void)ssid;
    (void)pass;
    syslog(LOG_INFO,
        "[%s] O74I: wifi_connect redirected to rpmsg_init\n", TAG);
    return network_rpmsg_init();
}

int network_wifi_reconnect(void)
{
    syslog(LOG_INFO,
        "[%s] O74I: wifi_reconnect redirected to rpmsg_init\n", TAG);
    return network_rpmsg_init();
}

#else
/* Real hardware: use NuttX wapi shell command to join WiFi */
#include "infra/config_store.h"
#include "agent_config.h"
#include <stdio.h>
#include <stdlib.h>

/*
 * Wrap src in single quotes for safe use inside a system() shell command.
 * Every embedded ' is replaced by the sequence '\'' so arbitrary SSID/PSK
 * strings (spaces, semicolons, ampersands, etc.) cannot break the command.
 * Returns 0 on success, -1 if buf is too small.
 */
static int shell_quote(const char* src, char* buf, size_t buf_size)
{
    size_t out = 0;
    if (buf_size < 3) return -1; /* need at least '' + NUL */
    buf[out++] = '\'';
    for (const char* p = src; *p; p++) {
        if (*p == '\'') {
            if (out + 4 >= buf_size) return -1;
            buf[out++] = '\'';
            buf[out++] = '\\';
            buf[out++] = '\'';
            buf[out++] = '\'';
        } else {
            if (out + 1 >= buf_size) return -1;
            buf[out++] = *p;
        }
    }
    if (out + 1 >= buf_size) return -1;
    buf[out++] = '\'';
    buf[out]   = '\0';
    return 0;
}

int network_wifi_connect(const char* iface, const char* ssid,
    const char* pass)
{
    if (!ssid || ssid[0] == '\0') {
        syslog(LOG_ERR, "[%s] wifi_connect: SSID required\n", TAG);
        return ERROR;
    }

    const char* dev = (iface && iface[0]) ? iface : "wlan0";
    char cmd[512];

    /* P2: single-quote ssid/pass so spaces and shell metacharacters in
     * Wi-Fi credentials cannot break or inject into the wapi commands. */
    char q_ssid[192], q_pass[384];
    if (shell_quote(ssid, q_ssid, sizeof(q_ssid)) != 0) {
        syslog(LOG_ERR, "[%s] wifi_connect: SSID too long or unquotable\n", TAG);
        return ERROR;
    }
    if (pass && pass[0] && shell_quote(pass, q_pass, sizeof(q_pass)) != 0) {
        syslog(LOG_ERR, "[%s] wifi_connect: PSK too long or unquotable\n", TAG);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Connecting WiFi SSID=%s on %s\n", TAG, ssid, dev);

    snprintf(cmd, sizeof(cmd), "ifup %s", dev); /* bring up iface first */
    system(cmd);
    usleep(500000); /* wait for driver ready */

    snprintf(cmd, sizeof(cmd), "wapi mode %s 2", dev); /* MANAGED */
    system(cmd);
    /* [WIFI-FIX] set country CN (channels 1-13) before associating.
     * esp_wifi default country is "01"/world = channels 1-11; Chinese APs
     * commonly use 12/13 which the default scan set cannot see -> NO_AP_FOUND.
     * CN (1-13) is a superset and safe on any channel. */
    snprintf(cmd, sizeof(cmd), "wapi country %s CN", dev);
    system(cmd);


    if (pass && pass[0]) {
        snprintf(cmd, sizeof(cmd), "wapi psk %s %s 3", dev, q_pass);
        system(cmd);
    }

    snprintf(cmd, sizeof(cmd), "wapi essid %s %s 1", dev, q_ssid);
    system(cmd);
    usleep(2000000); /* wait for AP association */

    /* P1: verify WiFi association before requesting DHCP.
     * 'wapi status' always exits 0 (it just prints iface state), so
     * checking its return code is useless. Instead, parse the output
     * for "Not-Associated" which wapi prints when no AP is joined.
     * If popen fails we fall through and let network_wait_connected
     * handle the timeout. */
    {
        bool associated = true;
        snprintf(cmd, sizeof(cmd), "wapi status %s", dev);
        FILE* fp = popen(cmd, "r");
        if (fp) {
            char line[256];
            while (fgets(line, sizeof(line), fp)) {
                if (strstr(line, "Not-Associated")) {
                    associated = false;
                    break;
                }
            }
            pclose(fp);
        }
        if (!associated) {
            syslog(LOG_ERR,
                "[%s] WiFi not associated (no carrier), skipping DHCP\n", TAG);
            return ERROR;
        }
    }

    snprintf(cmd, sizeof(cmd), "renew %s", dev); /* DHCP */
    system(cmd);

    /* Persist credentials.
     * P2: always write the PSK slot — even when pass is empty — so that
     * switching from a secured AP to an open one clears the stale PSK.
     * network_wifi_reconnect() would otherwise re-send the old wapi psk
     * command on the next boot, breaking reconnect for open networks. */
    agent_config_set(AGENT_CFG_KEY_WIFI_SSID, ssid);
    agent_config_set(AGENT_CFG_KEY_WIFI_PASS, (pass && pass[0]) ? pass : "");

    int err = network_wait_connected(15000);
    if (err == OK)
        syslog(LOG_INFO, "[%s] WiFi connected: %s\n", TAG, network_get_ip());
    else
        syslog(LOG_WARNING, "[%s] No IP after 15s — check SSID/password\n", TAG);
    return err;
}

int network_wifi_reconnect(void)
{
    /* The NuttX-native netinit (CONFIG_NETINIT_WAPI_SSID + CONFIG_NETINIT_
     * DHCPC, both set for this board) associates wlan0 with the AP and
     * obtains a DHCP lease during NSH bring-up.  If that already happened,
     * re-running the low-level 'wapi' sequence below would tear a healthy
     * association down and rebuild it (and, on the esp32s3 driver, may race
     * with netinit).  So first give netinit a few seconds to finish; if a
     * usable IPv4 address is present we are already online and must not
     * touch the interface.  Only fall back to the manual wapi path when the
     * link is still down (e.g. netinit failed, or the link dropped later). */

    if (network_wait_connected(8000) == OK) {
        syslog(LOG_INFO,
            "[%s] Network already up: %s (netinit handled WiFi)\n",
            TAG, network_get_ip());
        return OK;
    }

    char ssid[64] = { 0 };
    char pass[128] = { 0 };
    if (agent_config_get(AGENT_CFG_KEY_WIFI_SSID, ssid, sizeof(ssid)) != OK || !ssid[0]) {
        syslog(LOG_WARNING,
            "[%s] No saved WiFi credentials. Use CLI: set_wifi <ssid> <pass>\n",
            TAG);
        return ERROR;
    }
    agent_config_get(AGENT_CFG_KEY_WIFI_PASS, pass, sizeof(pass));
    return network_wifi_connect(NULL, ssid, pass);
}
#endif
