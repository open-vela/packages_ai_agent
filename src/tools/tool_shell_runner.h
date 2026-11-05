/* SPDX-License-Identifier: Apache-2.0 */
#ifndef TOOL_SHELL_RUNNER_H
#define TOOL_SHELL_RUNNER_H
#include <stddef.h>
#include <stdint.h>

/* Capture stdout without an unbounded fread/pclose wait. Returns 0 on exit
 * success, or a negative errno (including ETIMEDOUT and EOVERFLOW). */
int tool_shell_run(const char *command, char *out, size_t cap,
                   uint32_t timeout_ms);
#endif
