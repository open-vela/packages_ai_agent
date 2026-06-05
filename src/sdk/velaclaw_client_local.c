/*
 * velaclaw_client_local.c — Flat-build local IPC client for ai_agent
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
};

static struct velaclaw_client_s* g_client_instance;

static void tap_callback(const agent_msg_t* msg, void* cookie)
{
    struct velaclaw_client_s* c = (struct velaclaw_client_s*)cookie;
    if (!c) {
        return;
    }

    pthread_mutex_lock(&c->mtx);

    if (c->async_cb) {
        /* Async mode: invoke user callback directly */
        void (*cb)(int, const char*, void*) = c->async_cb;
        void* ck = c->async_cookie;
        c->async_cb = NULL;
        c->async_cookie = NULL;
        pthread_mutex_unlock(&c->mtx);

        cb(0, msg->content, ck);
        return;
    }

    /* Sync mode: store reply and signal */
    free(c->reply_text);
    c->reply_text = msg->content ? strdup(msg->content) : NULL;
    c->reply_status = 0;
    pthread_mutex_unlock(&c->mtx);

    sem_post(&c->reply_sem);
}

velaclaw_client_t* velaclaw_client_open(const char* name)
{
    if (g_client_instance) {
        return g_client_instance;
    }

    /* Check if message_bus is alive by trying a dummy operation */
    agent_msg_t probe = { 0 };
    strncpy(probe.channel, "__probe__", sizeof(probe.channel) - 1);

    /* We can't easily probe, so just assume agent is running if
     * mbus_tap_register succeeds (it uses the same global state) */

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

int velaclaw_ask(velaclaw_client_t* c,
    const velaclaw_ask_req_t* req,
    void (*cb)(int, const char*, void*), void* cookie)
{
    if (!c || !req || !req->text) {
        return -EINVAL;
    }

    /* Set up async callback */
    pthread_mutex_lock(&c->mtx);
    c->async_cb = cb;
    c->async_cookie = cookie;
    pthread_mutex_unlock(&c->mtx);

    /* Push message to agent inbound queue */
    agent_msg_t msg = { 0 };
    strncpy(msg.channel, CLIENT_CHANNEL, sizeof(msg.channel) - 1);
    strncpy(msg.chat_id, c->app_id, sizeof(msg.chat_id) - 1);
    msg.content = strdup(req->text);
    if (!msg.content) {
        return -ENOMEM;
    }

    int ret = message_bus_push_inbound(&msg);
    if (ret != OK) {
        free(msg.content);
        pthread_mutex_lock(&c->mtx);
        c->async_cb = NULL;
        c->async_cookie = NULL;
        pthread_mutex_unlock(&c->mtx);
        return -EIO;
    }

    return 0;
}
