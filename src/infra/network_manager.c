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
#include "infra/vela_tls.h" /* v125: 上行断开时作废旧 TLS 连接池（pool invalidate） */

#include <nuttx/net/dns.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

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

int network_wait_connected(uint32_t timeout_ms)
{
    if (timeout_ms == 0) {
        return network_is_connected() ? OK : ERROR;
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
            return OK;
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

#ifdef CONFIG_AI_AGENT_NET_RPMSG
/* ── QEMU stub: real NIC present, RPMSG state machine not needed ── */
/* The state-machine APIs below normally live in the RPMSG/TUN branch.
 * On QEMU the virtio-net NIC provides connectivity directly, so they
 * return sane defaults (connected, no resource limits). */

#include <errno.h>

static net_state_cb_t s_qemu_listeners[NET_MAX_LISTENERS];
static void *s_qemu_listener_args[NET_MAX_LISTENERS];
static int s_qemu_listener_count;

int network_register_listener(net_state_cb_t cb, void *arg)
{
    if (s_qemu_listener_count >= NET_MAX_LISTENERS)
        return -ENOMEM;
    s_qemu_listeners[s_qemu_listener_count] = cb;
    s_qemu_listener_args[s_qemu_listener_count] = arg;
    s_qemu_listener_count++;
    return OK;
}

net_state_t network_get_state(void)
{
    return network_is_connected() ? NET_STATE_CONNECTED
                                  : NET_STATE_DISCONNECTED;
}

int network_get_active_conns(void)
{
    return 0;
}

int network_get_iob_usage(void)
{
    return 0;
}

int network_rpmsg_init(void)
{
    return OK;
}

int network_reconnect(void)
{
    return network_wifi_reconnect();
}

int network_set_dns(const char *primary, const char *secondary)
{
    (void)primary;
    (void)secondary;
    return OK;
}

int network_diag(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    syslog(LOG_INFO, "[%s] QEMU: eth0 via virtio-net, state=%s\n", TAG,
           network_is_connected() ? "connected" : "disconnected");
    return OK;
}

int network_save_proxy_config(const char *mode, const char *cpu_name)
{
    (void)mode;
    (void)cpu_name;
    return OK;
}

int network_get_connect_timeout(void)
{
    return 15;
}

int network_get_read_timeout(void)
{
    return 30;
}

int network_get_retry_max(void)
{
    return 3;
}

int network_get_retry_base_sec(void)
{
    return 2;
}

const char *network_get_proxy_mode(void)
{
    return "usrsock";
}

const char *network_get_rpmsg_cpu(void)
{
    return "";
}

int network_acquire_resource(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return OK;
}

void network_release_resource(void)
{
}
#endif /* CONFIG_AI_AGENT_NET_RPMSG */

#elif defined(CONFIG_AI_AGENT_NET_RPMSG)
/* ── RPMSG/TUN network via BLE proxy ─────────────────────────── */

#include "config/config_store.h"

#ifdef CONFIG_AI_AGENT_BLE_NET
#include "ble_net.h"
#endif

#ifdef CONFIG_AI_AGENT_BLE_GATT
#include "ble_gatt_net.h"
#endif

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

/**
 * Check if bt-pan specifically has a valid IPv4 address.
 * Returns true if bt-pan is up with an IP (primary channel ready).
 */
static bool check_btpan_has_ip(void)
{
    struct ifaddrs* ifa_list = NULL;
    bool found = false;

    if (getifaddrs(&ifa_list) != 0) {
        return false;
    }

    for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (!ifa->ifa_name || strncmp(ifa->ifa_name, "bt-pan", 6) != 0) {
            continue;
        }

        struct sockaddr_in* sin = (struct sockaddr_in*)ifa->ifa_addr;
        uint32_t addr = ntohl(sin->sin_addr.s_addr);
        if (addr != 0 && (addr >> 24) != 127) {
            found = true;
        }
        break;
    }

    freeifaddrs(ifa_list);
    return found;
}

/**
 * Check if 4G PPP interface has a valid IPv4 address.
 *
 * 2026-10-26: 新增蜂窝(合宙 Air780E + netutils/pppd)通道。pppd 自己建
 * tun 网卡(名字固定 ppp0)并配 IP, 所以这里只按名字前缀 "ppp" + 有效 IPv4
 * 判定, 优先级设为最高(4G > bt-pan > BLE 备用)。
 */
static bool check_ppp_has_ip(void)
{
    struct ifaddrs* ifa_list = NULL;
    bool found = false;

    if (getifaddrs(&ifa_list) != 0) {
        return false;
    }

    for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (!ifa->ifa_name || strncmp(ifa->ifa_name, "ppp", 3) != 0) {
            continue;
        }

        struct sockaddr_in* sin = (struct sockaddr_in*)ifa->ifa_addr;
        uint32_t addr = ntohl(sin->sin_addr.s_addr);
        if (addr != 0 && (addr >> 24) != 127) {
            found = true;
            break;
        }
    }

    freeifaddrs(ifa_list);
    return found;
}

/**
 * Check if any non-bt-pan interface has a valid IPv4 address
 * (for BLE GATT NUS+TUN backup channel detection).
 */
static bool check_backup_channel_has_ip(void)
{
    struct ifaddrs* ifa_list = NULL;
    bool found = false;

    if (getifaddrs(&ifa_list) != 0) {
        return false;
    }

    for (struct ifaddrs* ifa = ifa_list; ifa; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET) {
            continue;
        }
        if (!ifa->ifa_name) continue;
        /* Skip loopback and bt-pan */
        if (strncmp(ifa->ifa_name, "lo", 2) == 0) continue;
        if (strncmp(ifa->ifa_name, "bt-pan", 6) == 0) continue;

        struct sockaddr_in* sin = (struct sockaddr_in*)ifa->ifa_addr;
        uint32_t addr = ntohl(sin->sin_addr.s_addr);
        if (addr != 0 && (addr >> 24) != 127) {
            found = true;
        }
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
        g_net_config.read_timeout = 30;
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

    /* 三通道优先级: 4G(ppp0) > bt-pan > BLE GATT(备用) */
    const char* active_channel = "none";

    while (g_poll_running) {
        /*
         * Priority: 4G cellular (ppp0, Air780E via netutils/pppd) > bt-pan >
         * BLE GATT. 4G 独立于手机, 是"离开手机也能上网"的主通道; bt-pan 作为
         * 手机在附近时的备用通道; BLE GATT 是最后的兜底。
         */
        bool ppp_ready = check_ppp_has_ip();
        bool btpan_ready = check_btpan_has_ip();
        bool backup_ready = false;
        bool has_ip = false;

        if (ppp_ready) {
            /* Highest priority: 4G cellular */
            has_ip = true;
            if (active_channel != "4g") {
                active_channel = "4g";
                syslog(LOG_INFO, "[%s] Active channel: 4G cellular (ppp0)\n", TAG);
            }
        } else if (btpan_ready) {
            /* Primary channel (bt-pan) is up — use it */
            has_ip = true;
            if (active_channel != "bt-pan") {
                active_channel = "bt-pan";
                syslog(LOG_INFO, "[%s] Active channel: bt-pan (primary)\n", TAG);
            }
        } else {
            /* Primary down — check backup channels */
#ifdef CONFIG_AI_AGENT_BLE_NET
            backup_ready = backup_ready || ble_net_is_connected();
#endif
#ifdef CONFIG_AI_AGENT_BLE_NET
            backup_ready = backup_ready || ble_gatt_net_is_connected();
#endif
            if (backup_ready && check_backup_channel_has_ip()) {
                has_ip = true;
                if (active_channel != "ble-gatt") {
                    active_channel = "ble-gatt";
                    syslog(LOG_INFO, "[%s] Active channel: BLE GATT (backup)\n", TAG);
                }
            } else {
                if (active_channel != "none") {
                    syslog(LOG_WARNING, "[%s] All channels down, was: %s\n",
                           TAG, active_channel);
                    active_channel = "none";
                }
            }
        }

        if (has_ip) {
            set_net_state(NET_STATE_CONNECTED);
            startup_warned = false;
        } else {
            /* v125：上行（bt-pan/4G）掉线时作废 TLS 连接池。
             * 池里的 keep-alive 连接此时已静默失效；空闲门限从 10s 放宽到
             * 120s 后，下一次请求很可能"复用一条死连接"：写可能进内核缓冲，
             * 读要等 SO_RCVTIMEO（network_get_read_timeout()=30s）才失败，
             * 用户侧就是一次 30s 卡顿。这里只在下线迁移时置一次标志，
             * 真正的 mbedtls 释放在下一次请求线程里做（见 vela_tls.c）。 */
            if (network_get_state() == NET_STATE_CONNECTED) {
                vela_tls_pool_invalidate();
            }
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

/* v125：IOB 水位读取（不加锁，供 network_acquire_resource 在持锁时调用）。
 *
 * 路径修正：NuttX 的 IOB 统计节点是 /proc/iobinfo，不是 /proc/net/iob。
 *   注册见 nuttx/fs/procfs/fs_procfs.c:145（"iobinfo" -> g_iobinfo_operations）；
 *   输出格式见 nuttx/fs/procfs/fs_procfsiobinfo.c:192-209：
 *       ntotal    nfree     nwait nthrottle
 *           64       48          0         0
 *   旧代码 fopen("/proc/net/iob") 永远打不开（真机日志
 *   .v3_work/perf/serial_perf_session.log:1112 起多处
 *   "[netmgr] Cannot open /proc/net/iob"），network_get_iob_usage() 恒返回 0，
 *   于是 NET_IOB_HIGH_WATERMARK=75 的限流（network_manager.h:78）一直没生效。
 *
 * 另外旧实现会在 fopen 成功后 pthread_mutex_lock(&g_net_mutex)，而唯一调用
 * 它的 network_acquire_resource() 本身就持有 g_net_mutex（非递归 mutex）——
 * 修好路径的同时必须拆掉这个自死锁，所以这里提供不加锁的读函数。
 *
 * 拿不到统计时返回 0（降级为非阻塞），保持原有"不因水位失败而卡网络"的语义。 */
static int iob_usage_pct_read(void)
{
    static const char* const k_paths[] = { "/proc/iobinfo", "/proc/net/iob" };
    static bool s_iob_warned;
    char line[80];
    int usage_pct = 0;

    for (size_t i = 0; i < sizeof(k_paths) / sizeof(k_paths[0]); i++) {
        int total = 0;
        int free_cnt = 0;
        FILE* fp = fopen(k_paths[i], "r");

        if (!fp) {
            continue;
        }

        /* 表头行（"ntotal nfree ..."）不满足 4 个整数，会被自然跳过。 */
        while (fgets(line, sizeof(line), fp)) {
            int ntotal;
            int nfree;
            int nwait;
            int nthrottle;

            if (sscanf(line, "%d %d %d %d", &ntotal, &nfree, &nwait, &nthrottle) == 4) {
                total = ntotal;
                free_cnt = nfree;
                break;
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
            return usage_pct;
        }
    }

    if (!s_iob_warned) {
        s_iob_warned = true;
        syslog(LOG_WARNING,
            "[%s] IOB stats unavailable (%s), IOB watermark disabled\n",
            TAG, k_paths[0]);
    }
    return 0;
}

int network_get_iob_usage(void)
{
    int usage_pct = iob_usage_pct_read();

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

    /* Register with the NuttX DNS resolver FIRST. CONFIG_NETDB_RESOLVCONF
     * is off on this board, so the resolver never reads /tmp/resolv.conf —
     * without this the domain lookup fails with
     * MBEDTLS_ERR_NET_UNKNOWN_HOST on every HTTPS request. */
    const char* servers[2] = { primary, secondary };
    int nservers = 2;

    /* v125：resolver 只有 1 个槽位时，先 primary 后 secondary 会把 primary 覆盖掉。
     *
     * 依据：本板 CONFIG_NETDB_DNSSERVER_NAMESERVERS=1
     *   （out/ble_align/.config:2189、out/ble_align/include/nuttx/config.h:593）。
     *   NuttX 无 CONFIG_NETDB_RESOLVCONF 分支的 dns_add_nameserver() 在槽满时
     *   直接覆盖 idx=0（nuttx/libs/libc/netdb/lib_dnsaddserver.c:319-328，写入
     *   见 :394-398）。所以 network_set_dns("223.5.5.5","8.8.8.8") 执行完后，
     *   resolver 里实际只剩 8.8.8.8 —— 国内手机 PAN 出口对 8.8.8.8 的 UDP/53
     *   通常不可达（本次无抓包证据），而 223.5.5.5 有实测可达性：
     *   serial_perf_session.log:139-145 ping 223.5.5.5 200/330ms（丢 1/3）；
     *   time_sync.c:50-52 也记录"223.5.5.5 是这条链路上稳定可达的解析器"。
     * 症状证据：resolver 失败时 nslookup fresh 域名报 "getaddrinfo failed: 1"
     *   （serial_perf_session.log:160；NuttX 把任何解析失败都折叠成
     *   HOST_NOT_FOUND=1，见 lib_gethostentbynamer.c:786-789），而已解析过的
     *   LLM 域名命中 DNS 缓存（NETDB_DNSCLIENT_ENTRIES=8 / LIFESEC=3600）仍能通
     *   （log:2543）。
     *
     * 单槽时只注册 primary（223.5.5.5）。PAN 起来后 panu_service.c:430-440 仍会
     * 用 DHCP 下发的 192.168.44.1 覆盖这个槽（这是正常且更优的顺序：实际出口的
     * DNS 最终生效），本改动只保证"DHCP DNS 尚未注册/没有 DNS 选项"的窗口里
     * resolver 指向的是可达的 223.5.5.5，而不是 8.8.8.8。
     * 多槽配置的行为保持不变。 */
#if defined(CONFIG_NETDB_DNSSERVER_NAMESERVERS) && (CONFIG_NETDB_DNSSERVER_NAMESERVERS < 2)
    nservers = 1;
    syslog(LOG_INFO,
        "[%s] DNS: resolver has %d slot, register primary %s only (skip %s)\n",
        TAG, CONFIG_NETDB_DNSSERVER_NAMESERVERS, primary,
        (secondary && secondary[0]) ? secondary : "-");
#endif

    for (int i = 0; i < nservers; i++) {
        struct sockaddr_in addr;

        if (!servers[i] || servers[i][0] == '\0') {
            continue;
        }
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        if (inet_pton(AF_INET, servers[i], &addr.sin_addr) != 1) {
            syslog(LOG_ERR, "[%s] set_dns: invalid addr %s\n",
                TAG, servers[i]);
            continue;
        }
        dns_add_nameserver((FAR const struct sockaddr*)&addr,
                           sizeof(addr));
    }

    /* The file write below is best-effort: /tmp may not exist and the
     * NuttX resolver does not read this file anyway. */
    fp = fopen("/tmp/resolv.conf", "w");
    if (!fp) {
        syslog(LOG_WARNING, "[%s] Cannot open /tmp/resolv.conf: %d "
            "(non-fatal, resolver registered directly)\n", TAG, errno);
    } else {
        fprintf(fp, "nameserver %s\n", primary);
        if (secondary && secondary[0] != '\0') {
            fprintf(fp, "nameserver %s\n", secondary);
        }
        fclose(fp);
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

#ifdef CONFIG_AI_AGENT_BLE_NET
    /* Start BLE SPP + TUN proxy channel (phone companion app) */
    ret = ble_net_init();
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] ble_net_init failed: %d\n", TAG, ret);
    } else {
        syslog(LOG_INFO, "[%s] BLE SPP+TUN proxy channel started\n", TAG);
    }
#endif

#ifdef CONFIG_AI_AGENT_BLE_NET
    /* Start BLE GATT NUS + TUN proxy channel (phone companion app).
     * Retry with backoff: bluetoothd may still be starting here (no BT
     * instance yet, or gatts registration fails), and without the
     * channel the phone can never connect. Each failed attempt cleans
     * up after itself, so retrying is safe. */
    for (int attempt = 0; attempt < 5; attempt++) {
        ret = ble_gatt_net_init();
        if (ret == 0) {
            syslog(LOG_INFO, "[%s] BLE GATT+TUN proxy channel started\n",
                TAG);
            break;
        }
        syslog(LOG_WARNING,
            "[%s] ble_gatt_net_init failed (%d), retry %d/5\n",
            TAG, ret, attempt + 1);
        sleep(3);
    }
#endif

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

    /* v125：这里必须用不加锁的 iob_usage_pct_read()。本函数已持有 g_net_mutex
     * （上面 pthread_mutex_lock），而公开的 network_get_iob_usage() 会再次
     * lock 同一把非递归 mutex → 自死锁。旧代码因为 /proc/net/iob 打不开、
     * 在读锁之前就 return 0，才没有暴露；路径修好后若继续调用会直接卡死。 */
    while (g_net_status.active_conns >= NET_MAX_TCP_CONNS
        || iob_usage_pct_read() >= NET_IOB_HIGH_WATERMARK) {
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
