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

#pragma once

/* The deadline rules live in the public header so an application that
 * streams a body uses the same arithmetic as the transport.  The vela_req_*
 * names are kept for the transport sources that were written against them. */

#include "agent_http.h"

#define VELA_REQ_IO_SLICE_MS AGENT_HTTP_IO_SLICE_MS

static inline int32_t vela_req_remaining_ms(uint32_t deadline_ms,
    uint32_t now_ms)
{
    return agent_http_remaining_ms(deadline_ms, now_ms);
}

static inline int32_t vela_req_budget_ms(const vela_req_ctl_t *ctl,
    uint32_t now_ms)
{
    return agent_http_budget_ms(ctl, now_ms);
}

static inline bool vela_req_cancelled(const vela_req_ctl_t *ctl)
{
    return agent_http_cancelled(ctl);
}

static inline bool vela_req_stop(const vela_req_ctl_t *ctl, uint32_t now_ms)
{
    return agent_http_stop(ctl, now_ms);
}

static inline uint32_t vela_req_io_timeout_ms(const vela_req_ctl_t *ctl,
    uint32_t now_ms, uint32_t cap_ms)
{
    return agent_http_io_timeout_ms(ctl, now_ms, cap_ms);
}
