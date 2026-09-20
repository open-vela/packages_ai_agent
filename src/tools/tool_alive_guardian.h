/*
 * Copyright (C) 2026
 *
 * Alive Guardian tool for the "Are You OK" application.
 */

#pragma once

#include <stddef.h>

int tool_alive_guardian_execute(const char *input_json,
                                char *output,
                                size_t output_size);
