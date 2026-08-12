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

#include "ble_gatt_net.h"
#include "ble_gatt.h"
#include "agent_compat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <pthread.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <netutils/netlib.h>
#include <nuttx/net/tun.h>

#include <uv.h>

#define TAG "ble_gatt_net"

/* TUN device */
#define TUN_DEV_NAME "bt-gatt"
#define TUN_DEV_IPADDR "192.168.55.2"
#define TUN_DEV_NETMASK "255.255.255.0"

/* Local name in the BLE advertisement */
#define BLE_NET_BT_NAME "Agent-Watch"

/* Frame protocol: [len_hi][len_lo][payload] big-endian 16-bit length
 * (identical to ble_net.c M1 framing) */
#define SPP_FRAME_HDR_SIZE 2
#define RX_FRAME_BUF_SIZE 4096
#define READ_BUF_SIZE 2048

/* -- State --------------------------------------------------- */

static struct {
    /* TUN */
    int tun_fd;
    uint8_t* tun_buf;
    size_t tun_mtu;
    uv_poll_t* tun_poll;

    /* GATT channel (ble_gatt.c owns the NUS service + advertising) */
    bool connected;

    /* RX frame reassembly (uv callback thread only) */
    uint8_t rx_frame_buf[RX_FRAME_BUF_SIZE];
    size_t rx_frame_len;

    /* State */
    bool initialized;

    /* Thread safety */
    pthread_mutex_t lock;
} g_ble_gatt_net = {
    .tun_fd = -1,
    .connected = false,
    .initialized = false,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/* -- TUN Device ---------------------------------------------- */

static int tun_open(void)
{
    struct ifreq ifr;
    int ret;

    g_ble_gatt_net.tun_fd = open("/dev/tun", O_RDWR | O_CLOEXEC);
    if (g_ble_gatt_net.tun_fd < 0) {
        syslog(LOG_ERR, "[%s] Failed to open /dev/tun: %d\n", TAG, errno);
        return -errno;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strlcpy(ifr.ifr_name, TUN_DEV_NAME, IFNAMSIZ);

    ret = ioctl(g_ble_gatt_net.tun_fd, TUNSETIFF, &ifr);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] TUNSETIFF failed: %d\n", TAG, errno);
        close(g_ble_gatt_net.tun_fd);
        g_ble_gatt_net.tun_fd = -1;
        return -errno;
    }

    g_ble_gatt_net.tun_mtu = CONFIG_NET_TUN_PKTSIZE;
    return 0;
}

static void tun_close(void)
{
    if (g_ble_gatt_net.tun_fd >= 0) {
        close(g_ble_gatt_net.tun_fd);
        g_ble_gatt_net.tun_fd = -1;
    }
}

static void tun_set_up(bool up)
{
    if (up) {
        struct in_addr addr;

        addr.s_addr = inet_addr(TUN_DEV_IPADDR);
        netlib_set_ipv4addr(TUN_DEV_NAME, &addr);
        addr.s_addr = inet_addr(TUN_DEV_NETMASK);
        netlib_set_ipv4netmask(TUN_DEV_NAME, &addr);
        netlib_ifup(TUN_DEV_NAME);
        syslog(LOG_INFO, "[%s] TUN %s up (%s)\n", TAG, TUN_DEV_NAME,
            TUN_DEV_IPADDR);
    } else {
        netlib_ifdown(TUN_DEV_NAME);
    }
}

static void ble_gatt_net_send_packet(const uint8_t* data, uint16_t len);

static void ble_gatt_net_send_packet(const uint8_t* data, uint16_t len)
{
    bool connected;

    pthread_mutex_lock(&g_ble_gatt_net.lock);
    connected = g_ble_gatt_net.connected;
    pthread_mutex_unlock(&g_ble_gatt_net.lock);

    if (!connected) {
        return;
    }

    /* Frame: [len_hi][len_lo][payload] */
    uint8_t* frame = malloc(SPP_FRAME_HDR_SIZE + len);
    if (!frame) {
        syslog(LOG_ERR, "[%s] send frame alloc failed\n", TAG);
        return;
    }
    frame[0] = (uint8_t)(len >> 8);
    frame[1] = (uint8_t)(len & 0xff);
    memcpy(frame + SPP_FRAME_HDR_SIZE, data, len);

    int ret = ble_gatt_send(frame, SPP_FRAME_HDR_SIZE + len);
    free(frame);

    if (ret < 0) {
        syslog(LOG_WARNING, "[%s] ble_gatt_send failed: %d\n", TAG, ret);
    }
}

static void tun_poll_cb(uv_poll_t* handle, int status, int events)
{
    (void)handle;

    if (status != 0) {
        syslog(LOG_ERR, "[%s] TUN poll error: %d\n", TAG, status);
        return;
    }

    if (events & UV_READABLE) {
        ssize_t len;
        uint8_t send_buf[READ_BUF_SIZE];
        bool should_send = false;

        pthread_mutex_lock(&g_ble_gatt_net.lock);
        if (!g_ble_gatt_net.initialized || g_ble_gatt_net.tun_fd < 0
            || !g_ble_gatt_net.tun_buf || g_ble_gatt_net.tun_mtu == 0) {
            pthread_mutex_unlock(&g_ble_gatt_net.lock);
            return;
        }

        len = read(g_ble_gatt_net.tun_fd, g_ble_gatt_net.tun_buf,
            g_ble_gatt_net.tun_mtu);
        if (len > 0 && g_ble_gatt_net.connected) {
            size_t copy_len = (size_t)len < sizeof(send_buf)
                ? (size_t)len : sizeof(send_buf);
            memcpy(send_buf, g_ble_gatt_net.tun_buf, copy_len);
            len = (ssize_t)copy_len;
            should_send = true;
        }
        pthread_mutex_unlock(&g_ble_gatt_net.lock);

        if (should_send) {
            ble_gatt_net_send_packet(send_buf, (uint16_t)len);
        }
    }
}

static void tun_poll_close_cb(uv_handle_t* handle)
{
    free(handle);
}

static int tun_poll_start(uv_loop_t* loop)
{
    g_ble_gatt_net.tun_poll = malloc(sizeof(uv_poll_t));
    if (!g_ble_gatt_net.tun_poll) {
        return -ENOMEM;
    }

    int ret = uv_poll_init(loop, g_ble_gatt_net.tun_poll,
        g_ble_gatt_net.tun_fd);
    if (ret < 0) {
        free(g_ble_gatt_net.tun_poll);
        g_ble_gatt_net.tun_poll = NULL;
        return ret;
    }

    ret = uv_poll_start(g_ble_gatt_net.tun_poll, UV_READABLE | UV_DISCONNECT,
        tun_poll_cb);
    if (ret < 0) {
        uv_close((uv_handle_t*)g_ble_gatt_net.tun_poll, tun_poll_close_cb);
        g_ble_gatt_net.tun_poll = NULL;
        return ret;
    }

    return 0;
}

static void tun_poll_stop(void)
{
    if (g_ble_gatt_net.tun_poll) {
        uv_poll_stop(g_ble_gatt_net.tun_poll);
        uv_close((uv_handle_t*)g_ble_gatt_net.tun_poll, tun_poll_close_cb);
        g_ble_gatt_net.tun_poll = NULL;
    }
}

/* -- Frame reassembly (phone -> TUN) -------------------------- */

static int tun_write_packet(const uint8_t* data, uint16_t len)
{
    int tun_fd;
    bool initialized;

    if (!data) {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_ble_gatt_net.lock);
    initialized = g_ble_gatt_net.initialized;
    tun_fd = g_ble_gatt_net.tun_fd;
    pthread_mutex_unlock(&g_ble_gatt_net.lock);

    if (!initialized || tun_fd < 0) {
        return -ENODEV;
    }

    if (len == 0 || len > g_ble_gatt_net.tun_mtu) {
        return -EINVAL;
    }

    ssize_t ret = write(tun_fd, data, len);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] TUN write failed: %d\n", TAG, errno);
        return -errno;
    }

    return (int)ret;
}

static void ble_gatt_net_handle_rx_stream(const uint8_t* data, size_t size)
{
    if (!g_ble_gatt_net.initialized) {
        return;
    }

    if (g_ble_gatt_net.rx_frame_len + size
        > sizeof(g_ble_gatt_net.rx_frame_buf)) {
        syslog(LOG_WARNING, "[%s] RX frame buffer overflow, resyncing\n",
            TAG);
        g_ble_gatt_net.rx_frame_len = 0;
        return;
    }
    memcpy(g_ble_gatt_net.rx_frame_buf + g_ble_gatt_net.rx_frame_len,
        data, size);
    g_ble_gatt_net.rx_frame_len += size;

    size_t off = 0;
    while (g_ble_gatt_net.rx_frame_len - off >= SPP_FRAME_HDR_SIZE) {
        uint16_t plen = ((uint16_t)g_ble_gatt_net.rx_frame_buf[off] << 8)
            | g_ble_gatt_net.rx_frame_buf[off + 1];
        if (plen == 0 || plen > g_ble_gatt_net.tun_mtu) {
            syslog(LOG_ERR, "[%s] Invalid frame length %u, resyncing\n",
                TAG, plen);
            g_ble_gatt_net.rx_frame_len = 0;
            return;
        }
        if (g_ble_gatt_net.rx_frame_len - off < SPP_FRAME_HDR_SIZE + plen) {
            break;
        }
        tun_write_packet(g_ble_gatt_net.rx_frame_buf + off
            + SPP_FRAME_HDR_SIZE, plen);
        off += SPP_FRAME_HDR_SIZE + plen;
    }

    if (off > 0) {
        memmove(g_ble_gatt_net.rx_frame_buf,
            g_ble_gatt_net.rx_frame_buf + off,
            g_ble_gatt_net.rx_frame_len - off);
        g_ble_gatt_net.rx_frame_len -= off;
    }
}

/* -- GATT bridge callbacks ------------------------------------ */

static void gatt_recv_cb(const uint8_t* data, uint16_t len, void* user_data)
{
    (void)user_data;
    ble_gatt_net_handle_rx_stream(data, len);
}

static void gatt_conn_cb(bool connected, void* user_data)
{
    (void)user_data;
    pthread_mutex_lock(&g_ble_gatt_net.lock);
    g_ble_gatt_net.connected = connected;
    pthread_mutex_unlock(&g_ble_gatt_net.lock);

    if (connected) {
        tun_set_up(true);
        syslog(LOG_INFO, "[%s] GATT connected\n", TAG);
    } else {
        tun_set_up(false);
        syslog(LOG_INFO, "[%s] GATT disconnected\n", TAG);
    }
}

/* -- Public API ------------------------------------------------ */

int ble_gatt_net_init(void)
{
    int ret;
    uv_loop_t* loop;
    ble_gatt_config_t cfg;

    pthread_mutex_lock(&g_ble_gatt_net.lock);
    if (g_ble_gatt_net.initialized) {
        pthread_mutex_unlock(&g_ble_gatt_net.lock);
        return 0;
    }
    pthread_mutex_unlock(&g_ble_gatt_net.lock);

    syslog(LOG_INFO, "[%s] Initializing BLE GATT network channel\n", TAG);

    g_ble_gatt_net.initialized = true;

    ret = tun_open();
    if (ret != 0) {
        g_ble_gatt_net.initialized = false;
        return ret;
    }

    g_ble_gatt_net.tun_buf = malloc(g_ble_gatt_net.tun_mtu);
    if (!g_ble_gatt_net.tun_buf) {
        tun_close();
        g_ble_gatt_net.initialized = false;
        return -ENOMEM;
    }

    /* Start the GATT NUS channel (service + advertising) */
    memset(&cfg, 0, sizeof(cfg));
    cfg.device_name = BLE_NET_BT_NAME;
    cfg.recv_cb = gatt_recv_cb;
    cfg.conn_cb = gatt_conn_cb;
    cfg.user_data = NULL;

    ret = ble_gatt_init(&cfg);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] ble_gatt_init failed: %d\n", TAG, ret);
        free(g_ble_gatt_net.tun_buf);
        g_ble_gatt_net.tun_buf = NULL;
        tun_close();
        g_ble_gatt_net.initialized = false;
        return ret;
    }

    loop = uv_default_loop();
    ret = tun_poll_start(loop);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] tun poll start failed: %d\n", TAG, ret);
        free(g_ble_gatt_net.tun_buf);
        g_ble_gatt_net.tun_buf = NULL;
        tun_close();
        g_ble_gatt_net.initialized = false;
        return ret;
    }

    syslog(LOG_INFO, "[%s] BLE GATT network channel ready\n", TAG);
    return 0;
}

int ble_gatt_net_deinit(void)
{
    pthread_mutex_lock(&g_ble_gatt_net.lock);
    if (!g_ble_gatt_net.initialized) {
        pthread_mutex_unlock(&g_ble_gatt_net.lock);
        return 0;
    }
    g_ble_gatt_net.initialized = false;
    pthread_mutex_unlock(&g_ble_gatt_net.lock);

    tun_poll_stop();
    ble_gatt_deinit();

    g_ble_gatt_net.connected = false;
    g_ble_gatt_net.rx_frame_len = 0;
    tun_set_up(false);

    if (g_ble_gatt_net.tun_buf) {
        free(g_ble_gatt_net.tun_buf);
        g_ble_gatt_net.tun_buf = NULL;
    }
    tun_close();

    syslog(LOG_INFO, "[%s] BLE GATT network channel stopped\n", TAG);
    return 0;
}

bool ble_gatt_net_is_connected(void)
{
    bool connected;
    pthread_mutex_lock(&g_ble_gatt_net.lock);
    connected = g_ble_gatt_net.connected;
    pthread_mutex_unlock(&g_ble_gatt_net.lock);
    return connected;
}

int ble_gatt_net_send(const uint8_t* data, uint16_t len)
{
    bool connected;

    if (!data || len == 0) {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_ble_gatt_net.lock);
    connected = g_ble_gatt_net.connected;
    pthread_mutex_unlock(&g_ble_gatt_net.lock);

    if (!connected) {
        return -ENOTCONN;
    }

    /* Frame: [len_hi][len_lo][payload], then notify (chunked by ble_gatt) */
    uint8_t* frame = malloc(SPP_FRAME_HDR_SIZE + len);
    if (!frame) {
        return -ENOMEM;
    }
    frame[0] = (uint8_t)(len >> 8);
    frame[1] = (uint8_t)(len & 0xff);
    memcpy(frame + SPP_FRAME_HDR_SIZE, data, len);

    int ret = ble_gatt_send(frame, SPP_FRAME_HDR_SIZE + len);
    free(frame);

    return ret >= 0 ? (int)len : ret;
}

int ble_gatt_net_receive(const uint8_t* data, uint16_t len)
{
    return tun_write_packet(data, len);
}
