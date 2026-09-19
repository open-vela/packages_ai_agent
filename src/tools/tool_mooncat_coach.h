/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MoonCat active recovery coach tool adapter for openVela ai_agent.
 */

#pragma once

#include <stddef.h>

int tool_mooncat_coach_execute(const char *input_json, char *output,
                               size_t output_size);
