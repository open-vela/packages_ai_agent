#ifndef __VELACLAW_CLIENT_H
#define __VELACLAW_CLIENT_H

#include <agent_http.h>

typedef struct velaclaw_client_s velaclaw_client_t;

typedef struct {
    const char* text;
    int timeout_ms;
} velaclaw_ask_req_t;

velaclaw_client_t* velaclaw_client_open(const char* name);
void velaclaw_client_close(velaclaw_client_t* c);

/* Queue one ask.  Only one callback may be outstanding: issuing a second
 * ask before the first reply or timeout returns -EBUSY. Local requests are
 * independent sessions with unique reply IDs; expired/duplicate replies are
 * discarded. timeout_ms <= 0 uses 300 seconds.  The reply
 * callback runs on the agent's outbound dispatch thread, so it must not
 * block or call back into the agent. */
int velaclaw_ask(velaclaw_client_t* c,
    const velaclaw_ask_req_t* req,
    void (*cb)(int, const char*, void*), void* cookie);

/* Same, but first asks the agent loop to start.  The VelaGuard HMI build
 * runs the loop lazily (AGENT_VG_HMI_LAZY_LOOP): the request only raises a
 * flag that the network watcher turns into a real start about a second
 * later.  A message pushed straight onto the bus without this would sit
 * unread, so any caller that is not an interactive CLI prompt needs it. */
/* Release the client's pending slot without waiting for its reply.  The late
 * reply is dropped by the chat_id check; the caller uses this after giving up
 * on a round so the next question is not refused as busy. */
void velaclaw_client_abandon(velaclaw_client_t* c);

int velaclaw_ask_async(velaclaw_client_t* c,
    const velaclaw_ask_req_t* req,
    void (*cb)(int, const char*, void*), void* cookie);

/* Opt-in request control.  deadline_ms is the caller's existing monotonic
 * deadline, including recognition and queue time; it is never restarted. */
int velaclaw_ask_async_deadline(velaclaw_client_t *c,
    const velaclaw_ask_req_t *req, uint32_t deadline_ms,
    void (*cb)(int, const char *, void *), void *cookie);

/* Agent-loop lifetime handshake.  begin registers a cancellation flag owned
 * by the currently processing message.  end must run on every exit before
 * that flag leaves scope.  No message retains a pointer to a client's flag.
 * Returns 1 for controlled, 0 for legacy, negative for a stale local request. */
int velaclaw_request_begin(const char *chat_id, vela_req_ctl_t *ctl,
    volatile int *cancel);
void velaclaw_request_end(volatile int *cancel);
/* Mark a matching controlled reply as failed without changing bus frames. */
void velaclaw_request_failed(const char *chat_id, int status);

#endif
