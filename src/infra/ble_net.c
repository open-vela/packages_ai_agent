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

/**
 * BLE Network Channel - Minimal SPP + TUN implementation
 *
 * Based on miwear app_net.c + app_spp.c, stripped of miwear dependencies.
 * Provides network connectivity via Bluetooth SPP to a companion phone app.
 */

#include "ble_net.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <pthread.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <netutils/netlib.h>
#include <nuttx/net/tun.h>

#include <bluetooth.h>
#include <bt_addr.h>
#include <bt_spp.h>
#include <bt_uuid.h>
#include <euv_pipe.h>
#include <uv.h>

#define TAG "ble_net"

/* TUN device name */
#define TUN_DEV_NAME "bt-net"

/* SPP server channel number (1-28) */
#define SPP_SCN 3

/* Read buffer size */
#define READ_BUF_SIZE 2048

/* -- State --------------------------------------------------- */

static struct {
    /* TUN */
    int tun_fd;
    uint8_t* tun_buf;
    size_t tun_mtu;
    uv_poll_t* tun_poll;

    /* SPP */
    bt_instance_t* bt_ins;
    void* spp_handle;
    uint16_t spp_port;
    bool spp_connected;

    /* SPP Proxy Pipe */
    euv_pipe_t* pipe_handle;
    bool pipe_connected;

    /* State */
    bool initialized;

    /* Thread safety */
    pthread_mutex_t lock;
} g_ble_net = {
    .tun_fd = -1,
    .spp_connected = false,
    .pipe_connected = false,
    .initialized = false,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/* -- TUN Device ---------------------------------------------- */

static int tun_open(void)
{
    struct ifreq ifr;
    int ret;

    g_ble_net.tun_fd = open("/dev/tun", O_RDWR | O_CLOEXEC);
    if (g_ble_net.tun_fd < 0) {
        syslog(LOG_ERR, "[%s] Failed to open /dev/tun: %d\n", TAG, errno);
        return -errno;
    }

    memset(&ifr, 0, sizeof(ifr));
    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    strlcpy(ifr.ifr_name, TUN_DEV_NAME, IFNAMSIZ);

    ret = ioctl(g_ble_net.tun_fd, TUNSETIFF, (unsigned long)&ifr);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] ioctl TUNSETIFF failed: %d\n", TAG, errno);
        close(g_ble_net.tun_fd);
        g_ble_net.tun_fd = -1;
        return -errno;
    }

    syslog(LOG_INFO, "[%s] TUN device %s created\n", TAG, TUN_DEV_NAME);
    return 0;
}

static void tun_close(void)
{
    int fd;

    pthread_mutex_lock(&g_ble_net.lock);
    fd = g_ble_net.tun_fd;
    g_ble_net.tun_fd = -1;
    pthread_mutex_unlock(&g_ble_net.lock);

    if (fd >= 0) {
        netlib_ifdown(TUN_DEV_NAME);
        close(fd);
        syslog(LOG_INFO, "[%s] TUN device closed\n", TAG);
    }
}

static int tun_get_mtu(void)
{
    struct ifreq ifr;
    int sockfd;
    int ret;
    int mtu = 1400; /* default */

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        return mtu;
    }

    memset(&ifr, 0, sizeof(ifr));
    strlcpy(ifr.ifr_name, TUN_DEV_NAME, IFNAMSIZ);

    ret = ioctl(sockfd, SIOCGIFMTU, &ifr);
    close(sockfd);

    if (ret >= 0 && ifr.ifr_mtu > 0) {
        mtu = ifr.ifr_mtu;
    }

    return mtu;
}

static void tun_set_up(bool up)
{
    if (up) {
        netlib_ifup(TUN_DEV_NAME);
        syslog(LOG_INFO, "[%s] TUN %s up\n", TAG, TUN_DEV_NAME);
    } else {
        netlib_ifdown(TUN_DEV_NAME);
        syslog(LOG_INFO, "[%s] TUN %s down\n", TAG, TUN_DEV_NAME);
    }
}

/* -- TUN Poll (read IP packets, send via SPP) ---------------- */

static void tun_poll_cb(uv_poll_t* handle, int status, int events)
{
    (void)handle;

    if (status != 0) {
        syslog(LOG_ERR, "[%s] TUN poll error: %d\n", TAG, status);
        return;
    }

    if (events & UV_READABLE) {
        ssize_t len;
        uint8_t send_buf[2048]; /* Stack buffer for copy */
        bool should_send = false;

        /* Hold lock for the entire read to prevent deinit from
         * freeing tun_buf / closing tun_fd while we're using them. */
        pthread_mutex_lock(&g_ble_net.lock);

        if (!g_ble_net.initialized || g_ble_net.tun_fd < 0
            || !g_ble_net.tun_buf || g_ble_net.tun_mtu == 0) {
            pthread_mutex_unlock(&g_ble_net.lock);
            return;
        }

        len = read(g_ble_net.tun_fd, g_ble_net.tun_buf, g_ble_net.tun_mtu);
        if (len > 0 && g_ble_net.spp_connected) {
            /* Copy to stack buffer so we can release the lock before send */
            size_t copy_len = (size_t)len < sizeof(send_buf)
                ? (size_t)len
                : sizeof(send_buf);
            if ((size_t)len > sizeof(send_buf)) {
                syslog(LOG_WARNING, "[%s] TUN packet %zd truncated to %zu\n",
                    TAG, len, sizeof(send_buf));
            }
            memcpy(send_buf, g_ble_net.tun_buf, copy_len);
            len = (ssize_t)copy_len;
            should_send = true;
        }

        pthread_mutex_unlock(&g_ble_net.lock);

        if (should_send) {
            ble_net_send(send_buf, (uint16_t)len);
        }
    }

    if (events & UV_DISCONNECT) {
        syslog(LOG_WARNING, "[%s] TUN disconnected\n", TAG);
    }
}

static void tun_poll_close_cb(uv_handle_t* handle)
{
    free(handle);
}

static int tun_poll_start(uv_loop_t* loop)
{
    g_ble_net.tun_poll = malloc(sizeof(uv_poll_t));
    if (!g_ble_net.tun_poll) {
        return -ENOMEM;
    }

    int ret = uv_poll_init(loop, g_ble_net.tun_poll, g_ble_net.tun_fd);
    if (ret < 0) {
        free(g_ble_net.tun_poll);
        g_ble_net.tun_poll = NULL;
        return ret;
    }

    ret = uv_poll_start(g_ble_net.tun_poll, UV_READABLE | UV_DISCONNECT,
        tun_poll_cb);
    if (ret < 0) {
        /* Handle was initialized by uv_poll_init, must use uv_close */
        uv_close((uv_handle_t*)g_ble_net.tun_poll, tun_poll_close_cb);
        g_ble_net.tun_poll = NULL;
        return ret;
    }

    return 0;
}

static void tun_poll_stop(void)
{
    if (g_ble_net.tun_poll) {
        uv_poll_stop(g_ble_net.tun_poll);
        uv_close((uv_handle_t*)g_ble_net.tun_poll, tun_poll_close_cb);
        g_ble_net.tun_poll = NULL;
    }
}

/* -- SPP Proxy Pipe ------------------------------------------ */

static void pipe_write_cb(euv_pipe_t* handle, uint8_t* buf, int status)
{
    (void)handle;
    if (status != 0) {
        syslog(LOG_ERR, "[%s] Pipe write failed: %d\n", TAG, status);
    }
    /* buf is always freed in callback, never in ble_net_send on error */
    if (buf) {
        free(buf);
    }
}

static void pipe_read_cb(euv_pipe_t* handle, const uint8_t* buf, ssize_t size)
{
    (void)handle;
    if (size > 0) {
        /* Received data from phone, write to TUN */
        ble_net_receive(buf, (uint16_t)size);
    } else if (size == 0) {
        /* EOF - peer closed the pipe */
        syslog(LOG_WARNING, "[%s] Pipe read EOF, disconnecting\n", TAG);
        pipe_disconnect();
        tun_set_up(false);
    } else {
        /* Read error */
        syslog(LOG_ERR, "[%s] Pipe read error: %zd, disconnecting\n",
            TAG, size);
        pipe_disconnect();
        tun_set_up(false);
    }
}

static void pipe_connect_cb(euv_pipe_t* handle, int status, void* user_data)
{
    (void)user_data;

    /* Guard: if deinit happened while connect was in-flight, discard */
    pthread_mutex_lock(&g_ble_net.lock);
    bool still_alive = g_ble_net.initialized;
    pthread_mutex_unlock(&g_ble_net.lock);

    if (!still_alive) {
        syslog(LOG_WARNING, "[%s] Late pipe_connect_cb after deinit, "
                            "discarding\n",
            TAG);
        euv_pipe_disconnect(handle);
        return;
    }

    if (status != 0) {
        syslog(LOG_ERR, "[%s] Pipe connect failed: %d\n", TAG, status);
        /* euv_pipe_connect allocates the handle before calling this callback.
         * On failure we must disconnect to free the pipe object. */
        euv_pipe_disconnect(handle);
        pthread_mutex_lock(&g_ble_net.lock);
        g_ble_net.pipe_handle = NULL;
        g_ble_net.pipe_connected = false;
        pthread_mutex_unlock(&g_ble_net.lock);
        return;
    }

    syslog(LOG_INFO, "[%s] SPP proxy pipe connected\n", TAG);

    /* Start reading from pipe (data from phone) */
    int ret = euv_pipe_read_start(handle, READ_BUF_SIZE, pipe_read_cb, NULL);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] Pipe read start failed: %d\n", TAG, ret);
        euv_pipe_disconnect(handle);
        pthread_mutex_lock(&g_ble_net.lock);
        g_ble_net.pipe_handle = NULL;
        g_ble_net.pipe_connected = false;
        pthread_mutex_unlock(&g_ble_net.lock);
        return;
    }

    /* Set both pipe_handle and pipe_connected atomically under lock */
    pthread_mutex_lock(&g_ble_net.lock);
    g_ble_net.pipe_handle = handle;
    g_ble_net.pipe_connected = true;
    pthread_mutex_unlock(&g_ble_net.lock);

    /* Bring up TUN interface */
    tun_set_up(true);
}

static void pipe_disconnect(void)
{
    euv_pipe_t* pipe;

    pthread_mutex_lock(&g_ble_net.lock);
    pipe = g_ble_net.pipe_handle;
    g_ble_net.pipe_handle = NULL;
    g_ble_net.pipe_connected = false;
    pthread_mutex_unlock(&g_ble_net.lock);

    /* Only disconnect if we had a valid handle */
    if (pipe) {
        euv_pipe_disconnect(pipe);
    }
}

/* -- SPP Callbacks ------------------------------------------- */

static void spp_connection_state_cb(void* handle, bt_address_t* addr,
    uint16_t scn, uint16_t port,
    profile_connection_state_t state)
{
    (void)handle;
    (void)scn;
    char addr_str[18];

    if (!addr) {
        syslog(LOG_ERR, "[%s] SPP connection callback with NULL addr\n", TAG);
        return;
    }
    bt_addr_ba2str(addr, addr_str);

    if (state == PROFILE_STATE_CONNECTED) {
        syslog(LOG_INFO, "[%s] SPP connected: %s port %d\n",
            TAG, addr_str, port);
        pthread_mutex_lock(&g_ble_net.lock);
        g_ble_net.spp_port = port;
        g_ble_net.spp_connected = true;
        pthread_mutex_unlock(&g_ble_net.lock);
        /* TUN will be brought up when proxy pipe connects */
    } else if (state == PROFILE_STATE_DISCONNECTED) {
        syslog(LOG_INFO, "[%s] SPP disconnected: %s\n", TAG, addr_str);
        pthread_mutex_lock(&g_ble_net.lock);
        g_ble_net.spp_connected = false;
        pthread_mutex_unlock(&g_ble_net.lock);
        pipe_disconnect();
        tun_set_up(false);
    }
}

static void spp_proxy_state_cb(void* handle, bt_address_t* addr,
    spp_proxy_state_t state, uint16_t scn,
    uint16_t port, char* name)
{
    (void)handle;
    (void)scn;
    (void)port;
    char addr_str[18];

    if (!addr) {
        syslog(LOG_ERR, "[%s] SPP proxy callback with NULL addr\n", TAG);
        return;
    }
    bt_addr_ba2str(addr, addr_str);

    if (state == SPP_PROXY_STATE_CONNECTED) {
        syslog(LOG_INFO, "[%s] SPP proxy connected: %s name=%s\n",
            TAG, addr_str, name ? name : "(null)");
        if (!name) {
            syslog(LOG_ERR, "[%s] SPP proxy name is NULL\n", TAG);
            return;
        }

        /* Guard: skip if not initialized or already connecting/connected */
        pthread_mutex_lock(&g_ble_net.lock);
        if (!g_ble_net.initialized) {
            pthread_mutex_unlock(&g_ble_net.lock);
            syslog(LOG_WARNING, "[%s] SPP proxy event after deinit, ignoring\n", TAG);
            return;
        }
        if (g_ble_net.pipe_connected || g_ble_net.pipe_handle) {
            pthread_mutex_unlock(&g_ble_net.lock);
            syslog(LOG_WARNING, "[%s] Duplicate proxy connect, ignoring\n", TAG);
            return;
        }
        pthread_mutex_unlock(&g_ble_net.lock);

        /* Copy name since BT stack may free it after callback returns */
        char* name_copy = strdup(name);
        if (!name_copy) {
            syslog(LOG_ERR, "[%s] Failed to copy proxy name\n", TAG);
            return;
        }

        /* Connect to the proxy pipe.
         * NOTE: euv_pipe_connect should be called from the libuv loop
         * thread.  On Vela/NuttX the BT callbacks and libuv typically
         * share the same event loop, so this is safe.  If ported to a
         * platform where they run on different threads, this call must
         * be marshalled via uv_async_send(). */
        uv_loop_t* loop = uv_default_loop();
        if (!loop) {
            syslog(LOG_ERR, "[%s] Failed to get uv default loop\n", TAG);
            free(name_copy);
            return;
        }
        /*
         * euv_pipe_connect copies the name internally (confirmed by
         * miwear app_spp.c which passes the stack-owned name directly).
         * We strdup + free to be safe in case BT stack frees name
         * before euv_pipe_connect reads it.
         */
        euv_pipe_t* pipe = euv_pipe_connect(loop, name_copy, pipe_connect_cb, NULL);
        free(name_copy);
        if (!pipe) {
            syslog(LOG_ERR, "[%s] Failed to connect proxy pipe\n", TAG);
            return;
        }
        /* pipe_handle will be set in pipe_connect_cb after connection succeeds */
    } else if (state == SPP_PROXY_STATE_DISCONNECTED) {
        syslog(LOG_INFO, "[%s] SPP proxy disconnected: %s\n", TAG, addr_str);
        pipe_disconnect();
        tun_set_up(false);
    }
}

static const spp_callbacks_t g_spp_cbs = {
    .size = sizeof(spp_callbacks_t),
    .connection_state_cb = spp_connection_state_cb,
    .proxy_state_cb = spp_proxy_state_cb,
    .pty_open_cb = NULL,
};

/* -- SPP Server ---------------------------------------------- */

static int spp_server_start(void)
{
    bt_uuid_t uuid;
    bt_status_t status;

    /* Get bluetooth instance */
    g_ble_net.bt_ins = bluetooth_get_instance();
    if (!g_ble_net.bt_ins) {
        syslog(LOG_ERR, "[%s] Failed to get BT instance\n", TAG);
        return -ENODEV;
    }

    /* Register SPP app */
    g_ble_net.spp_handle = bt_spp_register_app(g_ble_net.bt_ins, &g_spp_cbs);
    if (!g_ble_net.spp_handle) {
        syslog(LOG_ERR, "[%s] Failed to register SPP app\n", TAG);
        return -ENOMEM;
    }

    /* Start SPP server */
    bt_uuid16_create(&uuid, BT_UUID_SERVCLASS_SERIAL_PORT);
    status = bt_spp_server_start(g_ble_net.bt_ins, g_ble_net.spp_handle,
        SPP_SCN, &uuid, 1);
    if (status != BT_STATUS_SUCCESS) {
        syslog(LOG_ERR, "[%s] Failed to start SPP server: %d\n", TAG, status);
        bt_spp_unregister_app(g_ble_net.bt_ins, g_ble_net.spp_handle);
        g_ble_net.spp_handle = NULL;
        return -EIO;
    }

    syslog(LOG_INFO, "[%s] SPP server started on SCN %d\n", TAG, SPP_SCN);
    return 0;
}

static void spp_server_stop(void)
{
    if (g_ble_net.spp_handle) {
        bt_spp_server_stop(g_ble_net.bt_ins, g_ble_net.spp_handle, SPP_SCN);
        bt_spp_unregister_app(g_ble_net.bt_ins, g_ble_net.spp_handle);
        g_ble_net.spp_handle = NULL;
        syslog(LOG_INFO, "[%s] SPP server stopped\n", TAG);
    }
}

/* -- Public API ---------------------------------------------- */

int ble_net_init(void)
{
    int ret;
    uv_loop_t* loop;

    pthread_mutex_lock(&g_ble_net.lock);
    if (g_ble_net.initialized) {
        pthread_mutex_unlock(&g_ble_net.lock);
        return 0;
    }
    pthread_mutex_unlock(&g_ble_net.lock);

    syslog(LOG_INFO, "[%s] Initializing BLE network channel\n", TAG);

    /* Mark as initializing to prevent double-init race.
     * Will be cleared on failure. */
    g_ble_net.initialized = true;

    /* Get uv loop first */
    loop = uv_default_loop();
    if (!loop) {
        syslog(LOG_ERR, "[%s] Failed to get uv default loop\n", TAG);
        goto fail;
    }

    /* Open TUN device */
    ret = tun_open();
    if (ret < 0) {
        goto fail;
    }

    /* Get MTU and allocate buffer */
    g_ble_net.tun_mtu = tun_get_mtu();
    g_ble_net.tun_buf = malloc(g_ble_net.tun_mtu);
    if (!g_ble_net.tun_buf) {
        tun_close();
        goto fail;
    }

    /* Start TUN poll */
    ret = tun_poll_start(loop);
    if (ret < 0) {
        free(g_ble_net.tun_buf);
        g_ble_net.tun_buf = NULL;
        tun_close();
        goto fail;
    }

    /* Start SPP server */
    ret = spp_server_start();
    if (ret < 0) {
        tun_poll_stop();
        free(g_ble_net.tun_buf);
        g_ble_net.tun_buf = NULL;
        tun_close();
        goto fail;
    }

    pthread_mutex_lock(&g_ble_net.lock);
    /* Already set initialized=true at top; confirm under lock */
    pthread_mutex_unlock(&g_ble_net.lock);

    syslog(LOG_INFO, "[%s] BLE network channel ready (MTU=%zu)\n",
        TAG, g_ble_net.tun_mtu);
    return 0;

fail:
    g_ble_net.initialized = false;
    return ret ? ret : -EIO;
}

int ble_net_deinit(void)
{
    pthread_mutex_lock(&g_ble_net.lock);
    if (!g_ble_net.initialized) {
        pthread_mutex_unlock(&g_ble_net.lock);
        return 0;
    }
    g_ble_net.initialized = false;
    pthread_mutex_unlock(&g_ble_net.lock);

    pipe_disconnect();
    spp_server_stop();
    tun_poll_stop();

    if (g_ble_net.tun_buf) {
        free(g_ble_net.tun_buf);
        g_ble_net.tun_buf = NULL;
    }

    tun_close();

    syslog(LOG_INFO, "[%s] BLE network channel stopped\n", TAG);
    return 0;
}

bool ble_net_is_connected(void)
{
    bool connected;
    pthread_mutex_lock(&g_ble_net.lock);
    connected = g_ble_net.pipe_connected;
    pthread_mutex_unlock(&g_ble_net.lock);
    return connected;
}

int ble_net_send(const uint8_t* data, uint16_t len)
{
    euv_pipe_t* pipe;

    if (!data) {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_ble_net.lock);
    if (!g_ble_net.pipe_connected || !g_ble_net.pipe_handle) {
        pthread_mutex_unlock(&g_ble_net.lock);
        return -ENOTCONN;
    }
    pipe = g_ble_net.pipe_handle;
    pthread_mutex_unlock(&g_ble_net.lock);

    /* Allocate buffer for async write (freed in pipe_write_cb) */
    uint8_t* buf = malloc(len);
    if (!buf) {
        return -ENOMEM;
    }
    memcpy(buf, data, len);

    /*
     * euv_pipe_write: on success the callback frees buf.
     * On synchronous failure, callback is NOT invoked (confirmed by
     * euv_pipe.c:268), so we must free buf here.
     */
    int ret = euv_pipe_write(pipe, buf, len, pipe_write_cb);
    if (ret != 0) {
        syslog(LOG_ERR, "[%s] Pipe write failed: %d\n", TAG, ret);
        free(buf);
        return -EIO;
    }

    return len;
}

int ble_net_receive(const uint8_t* data, uint16_t len)
{
    int tun_fd;
    bool initialized;
    size_t tun_mtu;

    if (!data) {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_ble_net.lock);
    initialized = g_ble_net.initialized;
    tun_fd = g_ble_net.tun_fd;
    tun_mtu = g_ble_net.tun_mtu;
    pthread_mutex_unlock(&g_ble_net.lock);

    if (!initialized || tun_fd < 0) {
        return -ENODEV;
    }

    if (len == 0 || len > tun_mtu) {
        return -EINVAL;
    }

    ssize_t ret = write(tun_fd, data, len);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] TUN write failed: %d\n", TAG, errno);
        return -errno;
    }

    return (int)ret;
}
