#ifndef __VELACLAW_CLIENT_H
#define __VELACLAW_CLIENT_H

typedef struct velaclaw_client_s velaclaw_client_t;

typedef struct {
    const char* text;
    int timeout_ms;

    /* Optional session id. When non-NULL and non-empty, ai_agent keys this
     * turn's conversation history under this chat_id instead of the client's
     * app_id. Used by the on-device app to keep a long-running story/RPG
     * session's history separate from ordinary free chat. NULL or "" means
     * "use the client's app_id" (the default, unchanged behaviour). */
    const char* chat_id;
} velaclaw_ask_req_t;

velaclaw_client_t* velaclaw_client_open(const char* name);
void velaclaw_client_close(velaclaw_client_t* c);
int velaclaw_ask(velaclaw_client_t* c,
    const velaclaw_ask_req_t* req,
    void (*cb)(int, const char*, void*), void* cookie);

/* Register a persistent callback for UNSOLICITED inbound messages pushed to
 * this client's channel (e.g. cron-fired reminders) while no velaclaw_ask is
 * in flight. When async_cb is set (an ask is pending), ask replies take
 * precedence and notify_cb is not called. `status` is 1 for a partial
 * streaming fragment, 0 for the final/complete message. */
void velaclaw_set_notify_callback(velaclaw_client_t* c,
    void (*cb)(int, const char*, void*), void* cookie);

/* Publish an arbitrary message onto the ai_agent outbound bus, addressed to a
 * specific channel/chat_id (e.g. channel "mqtt" to reach a parent's MQTT
 * session). Used by the on-device app to report events — such as a child
 * confirming a reminder — back to a remote channel. Returns 0 on success. */
int velaclaw_publish(velaclaw_client_t* c,
    const char* channel, const char* chat_id, const char* text);

#endif
