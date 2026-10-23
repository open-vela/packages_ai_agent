/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t submitted, written, lost, pending;
    bool writer_running;
} tool_audit_stats_t;

void tool_audit_submit(const char *name, const char *input_json, int rc);
void tool_audit_get_stats(tool_audit_stats_t *out);
/* 仅供诊断等待落盘；语音和工具调用不等待磁盘。 */
int tool_audit_flush(uint32_t timeout_ms);
