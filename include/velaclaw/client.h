#ifndef __VELACLAW_CLIENT_H
#define __VELACLAW_CLIENT_H

typedef struct velaclaw_client_s velaclaw_client_t;

typedef struct {
    const char* text;
    int timeout_ms;
} velaclaw_ask_req_t;

velaclaw_client_t* velaclaw_client_open(const char* name);
void velaclaw_client_close(velaclaw_client_t* c);

/* Queue one ask.  Only one callback may be outstanding: issuing a second
 * ask before the first reply arrives replaces the callback.  The reply
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
int velaclaw_ask_async(velaclaw_client_t* c,
    const velaclaw_ask_req_t* req,
    void (*cb)(int, const char*, void*), void* cookie);

#endif
