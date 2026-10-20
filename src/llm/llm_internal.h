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
 * llm_internal.h — Shared types and helpers for the LLM subsystem.
 *
 * This header is internal to src/llm/ and must NOT be included
 * by code outside this directory.
 */

#include "cJSON.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "infra/agent_timing.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

/* ── Growable response buffer ─────────────────────────────── */

typedef struct {
    char* data;
    size_t len;
    size_t cap;
} resp_buf_t;

int resp_buf_init(resp_buf_t* rb, size_t initial_cap);
int resp_buf_append(resp_buf_t* rb, const char* data, size_t len);
void resp_buf_free(resp_buf_t* rb);

/* ── HTTP call (routes through direct or proxy path) ──────── */

int llm_http_call(const char* post_data, resp_buf_t* rb,
    int* out_status, agent_llm_timing_t* timing);

/* ── Config snapshot helpers ──────────────────────────────── */

void llm_snapshot_config(char* model, size_t model_sz,
    char* api_key, size_t key_sz,
    char* host, size_t host_sz);

const char* model_name_for_api(const char* model, const char* host);
bool is_openai_compat_host(const char* host);

/* ── JSON text extraction ─────────────────────────────────── */

void extract_text(cJSON* root, char* buf, size_t size);
