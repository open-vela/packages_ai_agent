/*
 * velaclaw_client_local.c - Flat-build local IPC client for ai_agent
 *
 * Uses direct function calls to message_bus (shared address space)
 * and mbus_tap to intercept outbound replies synchronously.
 */

#include <velaclaw/client.h>

#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <stdio.h>
#include <time.h>

#include "core/agent_loop.h"
#include "core/message_bus.h"
#include "core/message_bus_tap.h"

#define TAG "velaclaw_client"
#define CLIENT_CHANNEL "local_client"

struct velaclaw_client_s {
    char app_id[32];
    pthread_mutex_t mtx;
    sem_t reply_sem;
    char* reply_text;
    int reply_status;
    void (*async_cb)(int, const char*, void*);
    void* async_cookie;
    char pending_id[64];
    uint32_t serial;
    uint32_t started_ms;
    uint32_t timeout_ms;
    bool active;
};

static struct velaclaw_client_s* g_client_instance;

static uint32_t client_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static void tap_callback(const agent_msg_t* msg, void* cookie)
{
    struct velaclaw_client_s* c = cookie;
    if (!c || !msg || msg->is_progress) return;
    pthread_mutex_lock(&c->mtx);
    /* An old round can reply after its caller timed out and submitted a new
     * one. Correlate the bus reply, not just the current callback slot. */
    if (!c->active || strcmp(msg->chat_id, c->pending_id) != 0) {
        pthread_mutex_unlock(&c->mtx);
        return;
    }
    void (*cb)(int, const char*, void*) = c->async_cb;
    void* ck = c->async_cookie;
    bool expired = client_now_ms() - c->started_ms >= c->timeout_ms;
    c->active = false;
    c->async_cb = NULL;
    c->async_cookie = NULL;
    pthread_mutex_unlock(&c->mtx);
    if (cb && !expired) cb(0, msg->content, ck);
}

void velaclaw_client_abandon(velaclaw_client_t* c)
{
    if (!c) {
        return;
    }

    pthread_mutex_lock(&c->mtx);
    c->active = false;
    c->async_cb = NULL;
    c->async_cookie = NULL;
    pthread_mutex_unlock(&c->mtx);
}

velaclaw_client_t* velaclaw_client_open(const char* name)
{
    if (g_client_instance) {
        return g_client_instance;
    }

    struct velaclaw_client_s* c = calloc(1, sizeof(*c));
    if (!c) {
        return NULL;
    }

    strncpy(c->app_id, name ? name : "app", sizeof(c->app_id) - 1);
    pthread_mutex_init(&c->mtx, NULL);
    sem_init(&c->reply_sem, 0, 0);
    c->reply_text = NULL;
    c->reply_status = 0;

    if (mbus_tap_register(CLIENT_CHANNEL, tap_callback, c) != OK) {
        syslog(LOG_ERR, "[%s] tap register failed (agent not running?)\n", TAG);
        sem_destroy(&c->reply_sem);
        pthread_mutex_destroy(&c->mtx);
        free(c);
        return NULL;
    }

    g_client_instance = c;
    syslog(LOG_INFO, "[%s] client opened: %s\n", TAG, c->app_id);
    return c;
}

void velaclaw_client_close(velaclaw_client_t* c)
{
    if (!c) {
        return;
    }

    mbus_tap_unregister(CLIENT_CHANNEL);

    pthread_mutex_destroy(&c->mtx);
    sem_destroy(&c->reply_sem);
    free(c->reply_text);
    c->reply_text = NULL;

    if (g_client_instance == c) {
        g_client_instance = NULL;
    }

    free(c);
    syslog(LOG_INFO, "[%s] client closed\n", TAG);
}

int velaclaw_ask_async(velaclaw_client_t* c,
    const velaclaw_ask_req_t* req,
    void (*cb)(int, const char*, void*), void* cookie)
{
    if (!c || !req || !req->text) {
        return -EINVAL;
    }

    /* Under CONFIG_VG_HMI this only raises the start request; the network
     * watcher spins the loop up within about a second.  Other builds start
     * it outright.  Either way, ask before the message is queued so the
     * consumer exists by the time it lands. */
    agent_loop_ensure_started();

    return velaclaw_ask(c, req, cb, cookie);
}

int velaclaw_ask(velaclaw_client_t* c,
    const velaclaw_ask_req_t* req,
    void (*cb)(int, const char*, void*), void* cookie)
{
    if (!c || !req || !req->text) {
        return -EINVAL;
    }

    agent_msg_t msg = {0};
    msg.content = strdup(req->text);
    if (!msg.content) return -ENOMEM;
    pthread_mutex_lock(&c->mtx);
    uint32_t now = client_now_ms();
    if (c->active && now - c->started_ms < c->timeout_ms) {
        pthread_mutex_unlock(&c->mtx);
        free(msg.content);
        return -EBUSY;
    }
    c->serial++;
    snprintf(c->pending_id, sizeof(c->pending_id), "%s:%u",
             c->app_id, (unsigned)c->serial);
    c->started_ms = now;
    c->timeout_ms = req->timeout_ms > 0 ? (uint32_t)req->timeout_ms : 300000u;
    c->active = true;
    c->async_cb = cb;
    c->async_cookie = cookie;
    strcpy(msg.channel, CLIENT_CHANNEL);
    strcpy(msg.chat_id, c->pending_id);
    pthread_mutex_unlock(&c->mtx);

    int ret = message_bus_push_inbound(&msg);
    if (ret != OK) {
        free(msg.content);
        pthread_mutex_lock(&c->mtx);
        if (!strcmp(c->pending_id, msg.chat_id)) {
            c->active = false;
            c->async_cb = NULL;
            c->async_cookie = NULL;
        }
        pthread_mutex_unlock(&c->mtx);
        return -EIO;
    }
    return 0;
}
