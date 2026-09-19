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
#include <stddef.h>
#include <time.h>

/**
 * The correction to add to time() for this board's clock, in seconds.
 *
 * Zero until a network time has been obtained. Anything that reports or
 * reasons about wall-clock time should add this, so that a board whose RTC
 * is years out still says the right thing -- without the clock being moved,
 * which disturbs timers that are already armed.
 */
time_t tool_get_time_offset(void);

/**
 * Execute get_current_time tool.
 * Fetches current UTC time via HTTP Date response header,
 * sets the system clock, and returns a formatted local time string.
 */
int tool_get_time_execute(const char *input_json, char *output, size_t output_size);
