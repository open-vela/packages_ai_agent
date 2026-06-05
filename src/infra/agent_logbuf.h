/*
 * agent_logbuf.h — Ring buffer for agent logs (accessed by /api/logs)
 */

#pragma once

#include "cJSON.h"

/** Initialize the log ring buffer */
void agent_logbuf_init(void);

/** Push a formatted log line into the ring buffer */
void agent_logbuf_push(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

/** Dump all buffered logs as a cJSON array (caller must cJSON_Delete) */
cJSON* agent_logbuf_dump(void);
