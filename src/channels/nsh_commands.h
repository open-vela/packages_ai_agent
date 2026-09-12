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

#include "agent_compat.h"
#include <pthread.h>

/* Shared stdout mutex — prevents concurrent printf from outbound_dispatch_task
 * and cli_thread which causes adbd shell_service_uv assert. */
extern pthread_mutex_t g_stdout_lock;

/**
 * Register CLI commands (no thread spawned).
 * Safe to call before network is up.
 */
int nsh_commands_init(void);

/**
 * Spawn the stdin CLI thread.
 * Call after all services are in a known state (post Phase 5).
 */
int nsh_commands_start(void);

/**
 * True once the CLI thread has exited (user typed "quit" or stdin hit EOF).
 * Used by ai_agent_main so a CLI-only second instance can return to NSH
 * without triggering a full agent shutdown.
 */
bool nsh_cli_done(void);
