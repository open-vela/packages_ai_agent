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
 * skill_loader.h — Skills system for AI Agent (Vela/NuttX port)
 *
 */

#include "agent_compat.h"
#include <stddef.h>

/**
 * Initialize skills system.
 * Installs built-in skill files to the data directory if they don't
 * already exist.
 */
int skill_loader_init(void);

/**
 * Build a summary of all available skills for the system prompt.
 * Lists each skill with its title and description.
 *
 * @param buf   Output buffer
 * @param size  Buffer size
 * @return Number of bytes written (0 if no skills found)
 */
size_t skill_loader_build_summary(char *buf, size_t size);

/**
 * Check if skills directory has changed since last summary build.
 * Returns true if skills were added, removed, or modified.
 * Can be called periodically to trigger hot-reload.
 */
bool skill_loader_check_changed(void);

/**
 * Force rebuild of skills summary and invalidate tool registry.
 * Call after detecting skill changes to update system prompt
 * without restarting the process.
 */
void skill_loader_refresh(void);

/**
 * Truncate a UTF-8 string at a code point boundary, dropping any
 * trailing incomplete multi-byte sequence. Returns the sanitized
 * length. Use before any prompt fragment built by clamped string
 * concatenation reaches the LLM API.
 */
size_t utf8_sanitize(char *s, size_t len);
