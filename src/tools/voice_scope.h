/* SPDX-License-Identifier: Apache-2.0 */
#pragma once
#include <stdbool.h>

/* This product scope is selected only for controlled VelaGuard local asks.
 * Background and other clients keep their existing tool permissions. */
bool vg_voice_scope_request(const char *channel, const char *chat_id);
bool vg_voice_scope_tool(const char *name);
/* Owned JSON string; malformed input fails closed. NULL means allocation or
 * parse failure and the caller must fail the request without invoking LLM. */
char *vg_voice_scope_schema(const char *all_tools);
