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
 * BLE GATT Data Channel - Nordic UART Service (NUS) pattern
 *
 * Custom GATT service with RX (write) and TX (notify) characteristics
 * for bidirectional data exchange over BLE.
 */

#include "ble_gatt.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <advertiser_data.h>
#include <bluetooth.h>
#include <bt_addr.h>
#include <bt_gatt_defs.h>
#include <bt_gatts.h>
#include <bt_le_advertiser.h>
#include <bt_uuid.h>

#define TAG "ble_gatt"

/* NUS UUIDs as BT_UUID_DECLARE_128 inline bytes (little-endian) */
#define NUS_SVC_UUID_BYTES                          \
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
        0x93, 0xf3, 0xa3, 0xb5, 0x01, 0x00, 0x40, 0x6e
#define NUS_RX_UUID_BYTES                           \
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
        0x93, 0xf3, 0xa3, 0xb5, 0x02, 0x00, 0x40, 0x6e
#define NUS_TX_UUID_BYTES                           \
    0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, \
        0x93, 0xf3, 0xa3, 0xb5, 0x03, 0x00, 0x40, 0x6e

/* Attribute handle IDs (matching GATT_H_* macro convention) */
enum {
    NUS_SVC_ID = 1,
    NUS_TX_CHR_ID,
    NUS_TX_CCC_ID,
    NUS_RX_CHR_ID,
};

/* Default MTU */
#define DEFAULT_MTU 23

/* -- State --------------------------------------------------- */

static struct {
    bt_instance_t* bt_ins;
    gatts_handle_t srv_handle;

    bt_address_t peer_addr;
    bool connected;
    uint16_t mtu;
    bool notify_enabled;

    /* Advertising */
    bt_advertiser_t* adv_handle;
    bool advertising;

    ble_gatt_config_t config;

    bool initialized;
    pthread_mutex_t lock;
} g_gatt = {
    .connected = false,
    .mtu = DEFAULT_MTU,
    .notify_enabled = false,
    .adv_handle = NULL,
    .advertising = false,
    .initialized = false,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/* -- TX fragmentation queue ---------------------------------- *
 * BLE notifications carry at most (MTU - 3) bytes per ATT packet,
 * but TUN frames can be up to 1518 + 2 header bytes. ble_gatt_send()
 * splits every frame into (MTU-3)-byte chunks and paces them one at a
 * time on the framework's on_notify_complete callback, so the ATT TX
 * queue (CONFIG_BT_ATT_TX_COUNT) never overflows and no packet is
 * silently rejected with -EMSGSIZE (the pre-bletest behaviour: any
 * frame > MTU-3, i.e. every real IP packet on a default 23-byte MTU,
 * was dropped).
 */
#define TX_SLOT_SIZE 1600 /* 2-byte frame header + max IP packet */
#define TX_SLOT_COUNT 8   /* buffering: ~1s of link at 244B/30ms */

typedef struct {
    uint8_t data[TX_SLOT_SIZE];
    uint16_t len;
} tx_slot_t;

static struct {
    tx_slot_t slots[TX_SLOT_COUNT];
    uint8_t head;        /* next slot to transmit */
    uint8_t tail;        /* next free slot */
    uint8_t count;
    uint16_t off;        /* bytes already notified in the head slot */
    uint16_t last_chunk; /* chunk size of the in-flight notify */
    bool inflight;       /* a notify is awaiting its completion */
    pthread_mutex_t lock;
} g_txq = {
    .head = 0,
    .tail = 0,
    .count = 0,
    .off = 0,
    .last_chunk = 0,
    .inflight = false,
    .lock = PTHREAD_MUTEX_INITIALIZER,
};

/* -- Forward declarations ------------------------------------ */

static int adv_start(void);
static void tx_pump_locked(void); /* g_txq.lock must be held */

/* -- GATTS Callbacks ----------------------------------------- */

static void on_connected(gatts_handle_t srv_handle, bt_address_t* addr)
{
    char addr_str[18];

    if (!addr) {
        return;
    }
    bt_addr_ba2str(addr, addr_str);
    syslog(LOG_INFO, "[%s] GATT connected: %s\n", TAG, addr_str);

    pthread_mutex_lock(&g_gatt.lock);
    memcpy(&g_gatt.peer_addr, addr, sizeof(bt_address_t));
    g_gatt.connected = true;
    g_gatt.notify_enabled = false;
    /* Legacy advertising stops automatically on connection and the
     * framework sends no on_adv_stopped for legacy (that event is
     * ext-adv only): clear state here so a reconnect does not reuse
     * a stale advertiser handle. */
    g_gatt.advertising = false;
    g_gatt.adv_handle = NULL;
    pthread_mutex_unlock(&g_gatt.lock);

    /* Snapshot callback before releasing - avoids TOCTOU if config
     * is modified between the check and the call. */
    ble_gatt_conn_cb_t conn_cb;
    void* user_data;
    conn_cb = g_gatt.config.conn_cb;
    user_data = g_gatt.config.user_data;

    if (conn_cb) {
        conn_cb(true, user_data);
    }
}

static void on_disconnected(gatts_handle_t srv_handle, bt_address_t* addr)
{
    char addr_str[18];
    ble_gatt_conn_cb_t conn_cb;
    void* user_data;

    if (addr) {
        bt_addr_ba2str(addr, addr_str);
        syslog(LOG_INFO, "[%s] GATT disconnected: %s\n", TAG, addr_str);
    }

    pthread_mutex_lock(&g_gatt.lock);
    g_gatt.connected = false;
    g_gatt.notify_enabled = false;
    g_gatt.mtu = DEFAULT_MTU;
    conn_cb = g_gatt.config.conn_cb;
    user_data = g_gatt.config.user_data;
    pthread_mutex_unlock(&g_gatt.lock);

    /* Drop all queued TX frames: the ATT link is gone, a reconnect
     * must start from an empty queue. */
    pthread_mutex_lock(&g_txq.lock);
    g_txq.count = 0;
    g_txq.head = g_txq.tail = 0;
    g_txq.off = 0;
    g_txq.inflight = false;
    pthread_mutex_unlock(&g_txq.lock);

    if (conn_cb) {
        conn_cb(false, user_data);
    }

    /* Restart advertising so phone can reconnect (skip if deinit'd) */
    pthread_mutex_lock(&g_gatt.lock);
    bool still_init = g_gatt.initialized;
    pthread_mutex_unlock(&g_gatt.lock);

    if (still_init) {
        adv_start();
    }
}

static void on_mtu_changed(gatts_handle_t srv_handle, bt_address_t* addr,
    uint32_t mtu)
{
    /* The framework delivers the ATT payload (MTU - 3) here, not the
     * MTU itself. Store the real MTU so ble_gatt_send() subtracts 3
     * exactly once (the old code stored the payload and subtracted
     * another 3, rejecting 20-byte notifications on a default 23 MTU). */
    uint16_t real_mtu = (uint16_t)(mtu + 3);

    syslog(LOG_INFO, "[%s] MTU changed: %u\n", TAG, (unsigned)real_mtu);

    pthread_mutex_lock(&g_gatt.lock);
    g_gatt.mtu = real_mtu;
    pthread_mutex_unlock(&g_gatt.lock);
}

static void on_attr_table_added(gatts_handle_t srv_handle,
    gatt_status_t status, uint16_t attr_handle)
{
    syslog(LOG_INFO, "[%s] Attr table added, handle=0x%04x status=%d\n",
        TAG, attr_handle, status);
}

static void on_notify_complete(gatts_handle_t srv_handle, bt_address_t* addr,
    gatt_status_t status, uint16_t attr_handle)
{
    pthread_mutex_lock(&g_txq.lock);
    if (!g_txq.inflight) {
        pthread_mutex_unlock(&g_txq.lock);
        return;
    }
    g_txq.inflight = false;

    if (status != GATT_STATUS_SUCCESS) {
        /* No completion would ever arrive for this chunk (the zblue
         * stack only reports successes): drop the whole slot so the
         * stream keeps moving. TCP recovers the lost bytes. */
        syslog(LOG_ERR, "[%s] Notify failed, handle=0x%04x status=%d, "
            "dropping slot\n", TAG, attr_handle, status);
        if (g_txq.count > 0) {
            g_txq.count--;
            g_txq.head = (g_txq.head + 1) % TX_SLOT_COUNT;
        }
        g_txq.off = 0;
    } else if (g_txq.count > 0
        && g_txq.off >= g_txq.slots[g_txq.head].len) {
        /* Current slot fully notified: pop it and move to the next. */
        g_txq.count--;
        g_txq.head = (g_txq.head + 1) % TX_SLOT_COUNT;
        g_txq.off = 0;
    }

    tx_pump_locked();
    pthread_mutex_unlock(&g_txq.lock);
}

/* Send the next (MTU-3)-byte chunk of the head slot, if any.
 * Caller must hold g_txq.lock. */
static void tx_pump_locked(void)
{
    uint16_t mtu, max_payload, chunk;
    bt_status_t status;
    bt_address_t peer;
    gatts_handle_t srv;
    tx_slot_t* slot;

    if (g_txq.inflight || g_txq.count == 0) {
        return;
    }

    pthread_mutex_lock(&g_gatt.lock);
    if (!g_gatt.initialized || !g_gatt.connected || !g_gatt.notify_enabled) {
        pthread_mutex_unlock(&g_gatt.lock);
        /* Link went away while frames were queued: drop them all. */
        g_txq.count = 0;
        g_txq.head = g_txq.tail = 0;
        g_txq.off = 0;
        return;
    }
    srv = g_gatt.srv_handle;
    memcpy(&peer, &g_gatt.peer_addr, sizeof(bt_address_t));
    mtu = g_gatt.mtu;
    pthread_mutex_unlock(&g_gatt.lock);

    /* Loop so a failed submit falls through to the next slot
     * instead of stalling the queue. */
    while (g_txq.count > 0 && !g_txq.inflight) {
        slot = &g_txq.slots[g_txq.head];
        max_payload = (mtu > 3) ? (mtu - 3) : 20;
        chunk = slot->len - g_txq.off;
        if (chunk > max_payload) {
            chunk = max_payload;
        }

        status = bt_gatts_notify(srv, &peer, NUS_TX_CHR_ID,
            slot->data + g_txq.off, chunk);
        if (status != BT_STATUS_SUCCESS) {
            /* Service loop down / stack error: drop the slot to keep
             * the stream moving (TCP retransmits recover). */
            syslog(LOG_ERR, "[%s] Notify submit failed (%d), "
                "dropping slot\n", TAG, status);
            g_txq.count--;
            g_txq.head = (g_txq.head + 1) % TX_SLOT_COUNT;
            g_txq.off = 0;
            continue;
        }

        g_txq.inflight = true;
        g_txq.last_chunk = chunk;
        g_txq.off += chunk;
    }
}

/* RX Characteristic write callback - data from phone */
static uint16_t rx_char_on_write(gatts_handle_t srv_handle, bt_address_t* addr,
    uint16_t attr_handle, const uint8_t* value,
    uint16_t length, uint16_t offset)
{
    (void)srv_handle;
    (void)addr;
    (void)attr_handle;
    (void)offset;

    if (value && length > 0 && g_gatt.config.recv_cb) {
        g_gatt.config.recv_cb(value, length, g_gatt.config.user_data);
    }

    return length;
}

/* TX CCC write callback - enable/disable notifications */
static uint16_t tx_ccc_on_write(gatts_handle_t srv_handle, bt_address_t* addr,
    uint16_t attr_handle, const uint8_t* value,
    uint16_t length, uint16_t offset)
{
    (void)srv_handle;
    (void)addr;
    (void)attr_handle;
    (void)offset;

    if (!value || length < 2) {
        return 0; /* Reject short write */
    }

    uint16_t ccc_val = value[0] | (value[1] << 8);
    bool enabled = (ccc_val & 0x0001) != 0; /* Notification bit */

    pthread_mutex_lock(&g_gatt.lock);
    g_gatt.notify_enabled = enabled;
    pthread_mutex_unlock(&g_gatt.lock);

    syslog(LOG_INFO, "[%s] TX notify %s\n", TAG,
        enabled ? "enabled" : "disabled");

    return length;
}

static const gatts_callbacks_t g_gatts_cbs = {
    .size = sizeof(gatts_callbacks_t),
    .on_connected = on_connected,
    .on_disconnected = on_disconnected,
    .on_attr_table_added = on_attr_table_added,
    .on_attr_table_removed = NULL,
    .on_notify_complete = on_notify_complete,
    .on_mtu_changed = on_mtu_changed,
    .on_phy_read = NULL,
    .on_phy_updated = NULL,
    .on_conn_param_changed = NULL,
};

/* -- Service Setup (using GATT_H_* macros like in-tree examples) -- */

static gatt_attr_db_t s_nus_attr_db[] = {
    /* NUS Service */
    GATT_H_PRIMARY_SERVICE(BT_UUID_DECLARE_128(NUS_SVC_UUID_BYTES), NUS_SVC_ID),
    /* TX Characteristic (notify to phone) */
    GATT_H_CHARACTERISTIC_AUTO_RSP(BT_UUID_DECLARE_128(NUS_TX_UUID_BYTES),
        GATT_PROP_NOTIFY, 0, NULL, 0, NUS_TX_CHR_ID),
    /* TX CCC (enable/disable notifications) */
    GATT_H_CCCD(GATT_PERM_READ | GATT_PERM_WRITE,
        tx_ccc_on_write, NUS_TX_CCC_ID),
    /* RX Characteristic (write from phone) */
    GATT_H_CHARACTERISTIC_USER_RSP(BT_UUID_DECLARE_128(NUS_RX_UUID_BYTES),
        GATT_PROP_WRITE | GATT_PROP_WRITE_NR, GATT_PERM_WRITE,
        NULL, rx_char_on_write, NUS_RX_CHR_ID),
};

static gatt_srv_db_t s_nus_service_db = {
    .attr_db = s_nus_attr_db,
    .attr_num = sizeof(s_nus_attr_db) / sizeof(gatt_attr_db_t),
};

static int setup_nus_service(void)
{
    bt_status_t status;

    status = bt_gatts_add_attr_table(g_gatt.srv_handle, &s_nus_service_db);
    if (status != BT_STATUS_SUCCESS) {
        syslog(LOG_ERR, "[%s] Failed to add attr table: %d\n", TAG, status);
        return -EIO;
    }

    syslog(LOG_INFO, "[%s] NUS service registered\n", TAG);
    return 0;
}

/* -- BLE Advertising ------------------------------------------ */

#define BLE_GATT_ADV_NAME "Agent-Watch"
#define BLE_GATT_ADV_INTERVAL 320 /* 200ms (320 * 0.625ms) */
#define BLE_GATT_APPEARANCE 0x00C1 /* Watch: Sports Watch */

static void on_adv_start(bt_advertiser_t* adv, uint8_t adv_id,
    uint8_t status)
{
    if (status == BT_ADV_STATUS_SUCCESS) {
        syslog(LOG_INFO, "[%s] Advertising started (id=%u)\n", TAG, adv_id);
        pthread_mutex_lock(&g_gatt.lock);
        g_gatt.advertising = true;
        pthread_mutex_unlock(&g_gatt.lock);
    } else {
        syslog(LOG_ERR, "[%s] Advertising start failed: %u\n", TAG, status);
        /* On async failure (e.g. START_TIMEOUT) the framework has already
         * destroyed the advertiser: clear the dangling handle so adv_stop()
         * never touches freed memory. */
        pthread_mutex_lock(&g_gatt.lock);
        g_gatt.adv_handle = NULL;
        g_gatt.advertising = false;
        pthread_mutex_unlock(&g_gatt.lock);
    }
}

static void on_adv_stopped(bt_advertiser_t* adv, uint8_t adv_id)
{
    syslog(LOG_INFO, "[%s] Advertising stopped (id=%u)\n", TAG, adv_id);
    pthread_mutex_lock(&g_gatt.lock);
    g_gatt.advertising = false;
    g_gatt.adv_handle = NULL;
    pthread_mutex_unlock(&g_gatt.lock);
}

static const advertiser_callback_t g_adv_cbs = {
    .size = sizeof(advertiser_callback_t),
    .on_advertising_start = on_adv_start,
    .on_advertising_stopped = on_adv_stopped,
};

static int adv_start(void)
{
    pthread_mutex_lock(&g_gatt.lock);
    if (!g_gatt.initialized || !g_gatt.bt_ins) {
        pthread_mutex_unlock(&g_gatt.lock);
        return -ENODEV;
    }
    pthread_mutex_unlock(&g_gatt.lock);

    /* Build advertising data: flags + device name in the ADV packet so
     * the name is visible without a scan request (31-byte ADV limit:
     * name instead of the 128-bit service UUID; UUID goes in scan_rsp). */
    advertiser_data_t* adv_data = advertiser_data_new();
    if (!adv_data) {
        return -ENOMEM;
    }

    advertiser_data_set_flags(adv_data, 0x06); /* LE General Discoverable */
    advertiser_data_set_name(adv_data, BLE_GATT_ADV_NAME);

    uint16_t adv_len = 0;
    uint8_t* p_adv = advertiser_data_build(adv_data, &adv_len);

    /* Build scan response: NUS service UUID + appearance */
    advertiser_data_t* scan_rsp = advertiser_data_new();
    if (!scan_rsp) {
        advertiser_data_free(adv_data);
        return -ENOMEM;
    }
    bt_uuid_t svc_uuid;
    static const uint8_t svc_bytes[] = { NUS_SVC_UUID_BYTES };
    bt_uuid128_create(&svc_uuid, svc_bytes);
    advertiser_data_add_service_uuid(scan_rsp, &svc_uuid);
    advertiser_data_set_appearance(scan_rsp, BLE_GATT_APPEARANCE);

    uint16_t rsp_len = 0;
    uint8_t* p_rsp = advertiser_data_build(scan_rsp, &rsp_len);

    /* Set advertising parameters */
    ble_adv_params_t params;
    memset(&params, 0, sizeof(params));
    params.adv_type = BT_LE_LEGACY_ADV_IND; /* Legacy connectable undirected:
                                              * SF32LB52 LCPU firmware accepts
                                              * ext adv (0x2039) but transmits
                                              * nothing on air; legacy commands
                                              * (0x2006/0x2008/0x200A) go
                                              * through the controller directly. */
    params.own_addr_type = BT_LE_ADDR_TYPE_PUBLIC;
    params.interval = BLE_GATT_ADV_INTERVAL;
    params.channel_map = BT_LE_ADV_CHANNEL_DEFAULT;
    params.filter_policy = BT_LE_ADV_FILTER_WHITE_LIST_FOR_NONE;
    params.duration = 0; /* Advertise indefinitely */

    bt_advertiser_t* handle = bt_le_start_advertising(
        g_gatt.bt_ins, &params,
        p_adv, adv_len,
        p_rsp, rsp_len,
        (advertiser_callback_t*)&g_adv_cbs);

    advertiser_data_free(adv_data);
    advertiser_data_free(scan_rsp);

    if (!handle) {
        syslog(LOG_ERR, "[%s] Failed to start advertising\n", TAG);
        return -EIO;
    }

    pthread_mutex_lock(&g_gatt.lock);
    g_gatt.adv_handle = handle;
    pthread_mutex_unlock(&g_gatt.lock);

    syslog(LOG_INFO, "[%s] Advertising starting...\n", TAG);
    return 0;
}

static void adv_stop(void)
{
    pthread_mutex_lock(&g_gatt.lock);
    bt_advertiser_t* handle = g_gatt.adv_handle;
    g_gatt.adv_handle = NULL;
    g_gatt.advertising = false;
    pthread_mutex_unlock(&g_gatt.lock);

    if (handle && g_gatt.bt_ins) {
        bt_le_stop_advertising(g_gatt.bt_ins, handle);
    }
}

/* -- Advertising Retry ---------------------------------------- */

static void* adv_retry_thread(void* arg)
{
    (void)arg;
    int retries = 0;
    const int max_retries = 10;
    const int delay_sec = 3;

    while (retries < max_retries) {
        sleep(delay_sec);

        pthread_mutex_lock(&g_gatt.lock);
        bool done = g_gatt.advertising || !g_gatt.initialized;
        pthread_mutex_unlock(&g_gatt.lock);

        if (done) {
            if (g_gatt.advertising) {
                printf("[ble_gatt] advertising confirmed active\n");
            }
            break;
        }

        retries++;
        printf("[ble_gatt] adv retry %d/%d...\n", retries, max_retries);
        int ret = adv_start();
        if (ret == 0) {
            /* Wait for async callback to confirm */
            sleep(1);
            pthread_mutex_lock(&g_gatt.lock);
            bool ok = g_gatt.advertising;
            pthread_mutex_unlock(&g_gatt.lock);
            if (ok) {
                printf("[ble_gatt] advertising started on retry %d\n", retries);
                break;
            }
        }
    }

    if (retries >= max_retries) {
        printf("[ble_gatt] WARNING: advertising failed after %d retries\n",
            max_retries);
    }

    return NULL;
}

/* -- Public API ---------------------------------------------- */

int ble_gatt_init(const ble_gatt_config_t* config)
{
    bt_status_t status;
    int ret;

    if (!config || !config->recv_cb) {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_gatt.lock);
    if (g_gatt.initialized) {
        pthread_mutex_unlock(&g_gatt.lock);
        return 0;
    }
    pthread_mutex_unlock(&g_gatt.lock);

    printf("[ble_gatt] step 1: get BT instance\n");

    g_gatt.config = *config;

    /* Mark as initializing to prevent double-init race.
     * Will be cleared on failure. */
    g_gatt.initialized = true;

    /* Get BT instance */
    g_gatt.bt_ins = bluetooth_get_instance();
    if (!g_gatt.bt_ins) {
        printf("[ble_gatt] FAILED: no BT instance\n");
        goto fail;
    }
    printf("[ble_gatt] step 2: register GATT server\n");

    /* Register GATT server */
    status = bt_gatts_register_service(g_gatt.bt_ins, &g_gatt.srv_handle,
        (gatts_callbacks_t*)&g_gatts_cbs);
    if (status != BT_STATUS_SUCCESS) {
        printf("[ble_gatt] FAILED: register service: %d\n", status);
        goto fail;
    }
    printf("[ble_gatt] step 3: setup NUS service\n");

    /* Setup NUS service */
    ret = setup_nus_service();
    if (ret < 0) {
        printf("[ble_gatt] FAILED: setup NUS: %d\n", ret);
        bt_gatts_unregister_service(g_gatt.srv_handle);
        g_gatt.srv_handle = NULL;
        goto fail;
    }
    printf("[ble_gatt] step 4: start advertising\n");

    /* Start BLE advertising so phones can discover us */
    ret = adv_start();
    if (ret < 0) {
        printf("[ble_gatt] WARNING: advertising failed: %d, will retry\n", ret);
        /* Non-fatal: service works, just not discoverable via scan.
         * Start a retry thread to attempt advertising after BT is fully ready. */
    } else {
        printf("[ble_gatt] advertising started OK\n");
    }

    /* Retry thread disabled: re-starting advertising while the first
     * instance is active returns BT_ADV_STATUS_START_TIMEOUT and the
     * async on_adv_start callback may never arrive (async pipe issue),
     * causing useless repeated retries. First successful start is kept. */
#if 0
    {
        pthread_t retry_thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 2048);
        pthread_create(&retry_thread, &attr, adv_retry_thread, NULL);
        pthread_attr_destroy(&attr);
        pthread_detach(retry_thread);
    }
#endif

    /* Already marked initialized at the top; just log success */
    syslog(LOG_INFO, "[%s] BLE GATT channel ready\n", TAG);
    printf("[ble_gatt] init complete\n");
    return 0;

fail:
    g_gatt.initialized = false;
    return ret ? ret : -EIO;
}

int ble_gatt_deinit(void)
{
    pthread_mutex_lock(&g_gatt.lock);
    if (!g_gatt.initialized) {
        pthread_mutex_unlock(&g_gatt.lock);
        return 0;
    }
    g_gatt.initialized = false;
    g_gatt.connected = false;
    g_gatt.notify_enabled = false;
    pthread_mutex_unlock(&g_gatt.lock);

    pthread_mutex_lock(&g_txq.lock);
    g_txq.count = 0;
    g_txq.head = g_txq.tail = 0;
    g_txq.off = 0;
    g_txq.inflight = false;
    pthread_mutex_unlock(&g_txq.lock);

    adv_stop();

    if (g_gatt.srv_handle) {
        bt_gatts_unregister_service(g_gatt.srv_handle);
        g_gatt.srv_handle = NULL;
    }

    syslog(LOG_INFO, "[%s] BLE GATT channel stopped\n", TAG);
    return 0;
}

bool ble_gatt_is_connected(void)
{
    bool connected;
    pthread_mutex_lock(&g_gatt.lock);
    connected = g_gatt.connected;
    pthread_mutex_unlock(&g_gatt.lock);
    return connected;
}

int ble_gatt_send(const uint8_t* data, uint16_t len)
{
    tx_slot_t* slot;
    uint8_t next;

    if (!data || len == 0) {
        return -EINVAL;
    }
    if (len > TX_SLOT_SIZE) {
        syslog(LOG_ERR, "[%s] Frame %u exceeds TX slot %u\n",
            TAG, len, TX_SLOT_SIZE);
        return -EMSGSIZE;
    }

    pthread_mutex_lock(&g_gatt.lock);
    if (!g_gatt.initialized || !g_gatt.connected || !g_gatt.notify_enabled) {
        pthread_mutex_unlock(&g_gatt.lock);
        return -ENOTCONN;
    }
    pthread_mutex_unlock(&g_gatt.lock);

    pthread_mutex_lock(&g_txq.lock);
    if (g_txq.count == TX_SLOT_COUNT) {
        pthread_mutex_unlock(&g_txq.lock);
        syslog(LOG_WARNING, "[%s] TX queue full, dropping frame (%u B)\n",
            TAG, len);
        return -EBUSY;
    }

    slot = &g_txq.slots[g_txq.tail];
    memcpy(slot->data, data, len);
    slot->len = len;
    next = (g_txq.tail + 1) % TX_SLOT_COUNT;
    g_txq.tail = next;
    g_txq.count++;

    tx_pump_locked();
    pthread_mutex_unlock(&g_txq.lock);

    return (int)len;
}

uint16_t ble_gatt_get_mtu(void)
{
    uint16_t mtu;
    pthread_mutex_lock(&g_gatt.lock);
    mtu = g_gatt.mtu;
    pthread_mutex_unlock(&g_gatt.lock);
    return mtu;
}
