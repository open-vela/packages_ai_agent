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

/*
 * This file contains code derived from MimiClaw (https://github.com/memovai/mimiclaw)
 * Copyright (c) 2026 Ziboyan Wang, licensed under the MIT License.
 * See NOTICE file for the original MIT License terms.
 */

#pragma once
/**
 * message_bus.h — inter-task message bus
 *
 * with POSIX mutex + condition variable + ring buffer.
 */

#include "agent_compat.h"
#include <stdint.h>

/** Outbound message classes (agent_msg_t.kind).  Only a FINAL answer may be
 *  spoken aloud: the "working" status phrase and the error / low-memory
 *  notices still reach their channel (bubble, phone page), but they are
 *  progress chatter, not an answer - reading them out wastes a TTS round trip
 *  and makes the watch talk to itself.
 *  A zeroed message is FINAL, so every existing producer that does not care
 *  about this field keeps the old behaviour. */
enum {
    AGENT_MSG_KIND_FINAL = 0,  /**< a real answer: safe to speak */
    AGENT_MSG_KIND_STATUS = 1, /**< progress chatter ("正在思考中...") */
    AGENT_MSG_KIND_ERROR = 2,  /**< error / low-memory notice */
};

/** A single message travelling on the bus. */
typedef struct {
    char  channel[16];   /**< "feishu", "websocket", "cli", "system" */
    char  chat_id[64];   /**< chat_id (Feishu IDs are ~36 chars) */
    char *content;       /**< Heap-allocated text; receiver must free. */
    char *image_b64;     /**< Optional base64-encoded image; receiver must free. NULL if none. */
    uint8_t kind;        /**< Outbound only: AGENT_MSG_KIND_*; 0 (FINAL) when unset. */
    bool  from_local;    /**< Outbound only: the answer came from the on-device
                              model, not the cloud.  Lets the phone page and the
                              watch label the reply ("本地"/"云端") - otherwise
                              端云协同 is invisible without a serial log. */
} agent_msg_t;

/** Free heap members (content, image_b64) of a message.
 *  Safe to call on a zeroed or already-freed message. */
void message_bus_msg_free(agent_msg_t *msg);

/** Initialise inbound and outbound queues. */
int message_bus_init(void);

/** Destroy queues and release synchronisation resources. */
void message_bus_destroy(void);

/** Wake all threads blocked on pop (used during shutdown). */
void message_bus_wakeup(void);

/** Push a message to the inbound queue (towards the agent loop).
 *  On success the bus takes ownership of heap members.
 *  On failure (queue full / timeout) the caller still owns them. */
int message_bus_push_inbound(const agent_msg_t *msg);

/** Try to push inbound without waiting.  On success the bus owns heap members;
 *  on failure the caller retains ownership.  Use from render/UI threads. */
int message_bus_try_push_inbound(const agent_msg_t *msg);

/** Block until an inbound message is available (or timeout expires).
 *  Caller must free msg->content / msg->image_b64 when done. */
int message_bus_pop_inbound(agent_msg_t *msg, uint32_t timeout_ms);

/** Push a message to the outbound queue (towards channels).
 *  On success the bus takes ownership of heap members.
 *  On failure the caller still owns them. */
int message_bus_push_outbound(const agent_msg_t *msg);

/** Block until an outbound message is available (or timeout expires). */
int message_bus_pop_outbound(agent_msg_t *msg, uint32_t timeout_ms);
