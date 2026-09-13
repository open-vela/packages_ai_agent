#ifndef __VELACLAW_CLIENT_H
#define __VELACLAW_CLIENT_H

typedef struct velaclaw_client_s velaclaw_client_t;

typedef struct {
    const char* text;
    int timeout_ms;
} velaclaw_ask_req_t;

velaclaw_client_t* velaclaw_client_open(const char* name);
void velaclaw_client_close(velaclaw_client_t* c);
int velaclaw_ask(velaclaw_client_t* c,
    const velaclaw_ask_req_t* req,
    void (*cb)(int, const char*, void*), void* cookie);

#endif
