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

/**
 * llm_parse.c — Tool call parsing and response format conversion.
 *
 * Extracted from llm_proxy.c to isolate parsing logic:
 *   - build_openai_tools_array()    — internal to OpenAI format
 *   - parse_xml_tool_calls()        — XML fallback (Format 1)
 *   - parse_ns_xml_tool_calls()     — namespaced XML (Format 2)
 *   - extract_openai_tool_calls()   — OpenAI format extraction
 *   - llm_response_free()           — response cleanup
 */

#include "llm/llm_parse.h"
#include "llm/llm_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"

#include "cJSON.h"
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static const char* TAG = "llm_parse";

/* ── Tools format conversion ──────────────────────────────── */

cJSON* build_openai_tools_array(const char* tools_json)
{
    if (!tools_json) {
        return NULL;
    }

    cJSON* tools_spec = cJSON_Parse(tools_json);

    if (!tools_spec || !cJSON_IsArray(tools_spec)) {
        cJSON_Delete(tools_spec);
        return NULL;
    }

    cJSON* tools_arr = cJSON_CreateArray();
    cJSON* tool_def;

    cJSON_ArrayForEach(tool_def, tools_spec)
    {
        cJSON* wrapper = cJSON_CreateObject();

        cJSON_AddStringToObject(wrapper, "type", "function");

        cJSON* func = cJSON_CreateObject();
        cJSON* name = cJSON_GetObjectItem(tool_def, "name");
        cJSON* desc = cJSON_GetObjectItem(tool_def, "description");
        cJSON* schema = cJSON_GetObjectItem(tool_def, "input_schema");

        if (name && cJSON_IsString(name)) {
            cJSON_AddStringToObject(func, "name", name->valuestring);
        }
        if (desc && cJSON_IsString(desc)) {
            cJSON_AddStringToObject(func, "description",
                desc->valuestring);
        }
        if (schema) {
            cJSON_AddItemToObject(func, "parameters",
                cJSON_Duplicate(schema, 1));
        } else {
            cJSON* empty = cJSON_CreateObject();
            cJSON_AddStringToObject(empty, "type", "object");
            cJSON_AddItemToObject(empty, "properties",
                cJSON_CreateObject());
            cJSON_AddItemToObject(func, "parameters", empty);
        }

        cJSON_AddItemToObject(wrapper, "function", func);
        cJSON_AddItemToArray(tools_arr, wrapper);
    }

    cJSON_Delete(tools_spec);
    return tools_arr;
}

/* ── XML fallback parsers ─────────────────────────────────── */

/* Parse Format 1: <tool_call> <function=NAME> <parameter=KEY>VAL
 * </parameter> </function> </tool_call> */
void parse_xml_tool_calls(llm_response_t* resp)
{
    if (resp->call_count > 0 || !resp->text) {
        return;
    }
    if (!strstr(resp->text, "<tool_call>")) {
        return;
    }

    const char* p = resp->text;

    while (resp->call_count < AGENT_MAX_TOOL_CALLS) {
        const char* tc_start = strstr(p, "<tool_call>");
        const char* tc_end = strstr(p, "</tool_call>");

        if (!tc_start || !tc_end || tc_end <= tc_start) {
            break;
        }

        const char* fn_start = strstr(tc_start, "<function=");
        const char* fn_close = fn_start
            ? strchr(fn_start + 10, '>')
            : NULL;

        if (!fn_start || !fn_close || fn_start > tc_end) {
            p = tc_end + 12;
            continue;
        }

        llm_tool_call_t* call = &resp->calls[resp->call_count];
        size_t name_len = (size_t)(fn_close - (fn_start + 10));

        if (name_len >= sizeof(call->name)) {
            name_len = sizeof(call->name) - 1;
        }
        memcpy(call->name, fn_start + 10, name_len);
        call->name[name_len] = '\0';
        snprintf(call->id, sizeof(call->id), "xml_%d",
            resp->call_count);

        cJSON* args = cJSON_CreateObject();
        const char* pp = fn_close + 1;

        while (pp < tc_end) {
            const char* ps = strstr(pp, "<parameter=");

            if (!ps || ps >= tc_end) {
                break;
            }
            const char* ke = strchr(ps + 11, '>');

            if (!ke || ke >= tc_end) {
                break;
            }

            char key[64];
            size_t klen = (size_t)(ke - (ps + 11));

            if (klen >= sizeof(key)) {
                klen = sizeof(key) - 1;
            }
            memcpy(key, ps + 11, klen);
            key[klen] = '\0';

            const char* vs = ke + 1;
            const char* ve = strstr(vs, "</parameter>");

            if (!ve || ve > tc_end) {
                break;
            }

            size_t vlen = (size_t)(ve - vs);
            char* val = calloc(1, vlen + 1);

            if (val) {
                memcpy(val, vs, vlen);
                cJSON_AddStringToObject(args, key, val);
                free(val);
            }
            pp = ve + 12;
        }

        char* args_str = cJSON_PrintUnformatted(args);

        cJSON_Delete(args);
        if (args_str) {
            call->input = args_str;
            call->input_len = strlen(args_str);
        }

        resp->call_count++;
        p = tc_end + 12;
    }

    if (resp->call_count > 0) {
        resp->tool_use = true;
        free(resp->text);
        resp->text = NULL;
        resp->text_len = 0;
        syslog(LOG_WARNING,
            "[%s] Parsed %d tool call(s) from XML fallback\n",
            TAG, resp->call_count);
    }
}

/* Parse Format 2: <PREFIX:tool_call> <invoke name="NAME">
 * <parameter name="KEY">VAL</parameter> </invoke>
 * </PREFIX:tool_call> */
void parse_ns_xml_tool_calls(llm_response_t* resp)
{
    if (resp->call_count > 0 || !resp->text) {
        return;
    }
    if (!strstr(resp->text, ":tool_call>")) {
        return;
    }

    const char* p = resp->text;

    while (resp->call_count < AGENT_MAX_TOOL_CALLS) {
        const char* colon = strstr(p, ":tool_call>");

        if (!colon) {
            break;
        }

        const char* tc_start = colon;

        while (tc_start > p && *(tc_start - 1) != '<') {
            tc_start--;
        }
        if (tc_start <= p || *(tc_start - 1) != '<') {
            break;
        }
        tc_start--;

        size_t prefix_len = (size_t)(colon - (tc_start + 1));

        if (prefix_len == 0 || prefix_len > 31) {
            p = colon + 11;
            continue;
        }

        char prefix[32];

        memcpy(prefix, tc_start + 1, prefix_len);
        prefix[prefix_len] = '\0';

        char close_tag[64];

        snprintf(close_tag, sizeof(close_tag),
            "</%s:tool_call>", prefix);

        const char* tc_end = strstr(colon, close_tag);

        if (!tc_end || tc_end <= tc_start) {
            p = colon + 11;
            continue;
        }

        const char* inv = strstr(tc_start, "<invoke name=\"");

        if (!inv || inv > tc_end) {
            p = tc_end + strlen(close_tag);
            continue;
        }

        const char* ns = inv + 14;
        const char* ne = strchr(ns, '"');

        if (!ne || ne > tc_end) {
            p = tc_end + strlen(close_tag);
            continue;
        }

        llm_tool_call_t* call = &resp->calls[resp->call_count];
        size_t name_len = (size_t)(ne - ns);

        if (name_len >= sizeof(call->name)) {
            name_len = sizeof(call->name) - 1;
        }
        memcpy(call->name, ns, name_len);
        call->name[name_len] = '\0';
        snprintf(call->id, sizeof(call->id), "nsxml_%d",
            resp->call_count);

        cJSON* args = cJSON_CreateObject();
        const char* pp = ne;

        while (pp < tc_end) {
            const char* ps = strstr(pp, "<parameter name=\"");

            if (!ps || ps >= tc_end) {
                break;
            }

            const char* ks = ps + 17;
            const char* ke = strchr(ks, '"');

            if (!ke || ke >= tc_end) {
                break;
            }

            const char* tc = strchr(ke, '>');

            if (!tc || tc >= tc_end) {
                break;
            }

            char key[64];
            size_t klen = (size_t)(ke - ks);

            if (klen >= sizeof(key)) {
                klen = sizeof(key) - 1;
            }
            memcpy(key, ks, klen);
            key[klen] = '\0';

            const char* vs = tc + 1;
            const char* ve = strstr(vs, "</parameter>");

            if (!ve || ve > tc_end) {
                break;
            }

            size_t vlen = (size_t)(ve - vs);
            char* val = calloc(1, vlen + 1);

            if (val) {
                memcpy(val, vs, vlen);
                cJSON_AddStringToObject(args, key, val);
                free(val);
            }
            pp = ve + 12;
        }

        char* args_str = cJSON_PrintUnformatted(args);

        cJSON_Delete(args);
        if (args_str) {
            call->input = args_str;
            call->input_len = strlen(args_str);
        }

        resp->call_count++;
        p = tc_end + strlen(close_tag);
    }

    if (resp->call_count > 0) {
        resp->tool_use = true;
        free(resp->text);
        resp->text = NULL;
        resp->text_len = 0;
        syslog(LOG_WARNING,
            "[%s] Parsed %d tool call(s) from namespaced XML\n",
            TAG, resp->call_count);
    }
}

/* Parse Format 3: DeepSeek DSML.
 *
 *   <｜｜DSML｜｜ calls>
 *   <｜｜DSML｜｜ invoke name="NAME">
 *   <｜｜DSML｜｜ parameter name="KEY">VALUE</｜｜DSML｜｜ parameter>
 *   </｜｜DSML｜｜ invoke>
 *   <｜｜DSML｜｜ calls>
 *
 * DeepSeek puts the tool call in the message content instead of the
 * OpenAI tool_calls field, and the two existing XML parsers do not match
 * it: they look for "tool_call" and ":tool_call>", while DSML writes
 * "calls" and "invoke".  The namespace prefix is matched loosely here -
 * only "invoke name=", "parameter name=" and the closing tags matter - so
 * it does not matter whether the model spells the prefix with the
 * fullwidth pipes or with plain angle brackets. */
void parse_dsml_tool_calls(llm_response_t* resp)
{
    static const char INVOKE[] = "invoke name=\"";
    static const char PARAM[] = "parameter name=\"";
    static const char INVOKE_END[] = "invoke>";
    const char* p;

    if (resp->call_count > 0 || !resp->text) {
        return;
    }

    if (!strstr(resp->text, INVOKE)) {
        return;
    }

    p = resp->text;

    while (resp->call_count < AGENT_MAX_TOOL_CALLS) {
        const char* inv = strstr(p, INVOKE);
        const char* name_end;
        const char* inv_end;
        const char* scan;
        llm_tool_call_t* call;
        cJSON* args;

        if (!inv) {
            break;
        }

        name_end = strchr(inv + strlen(INVOKE), '"');
        if (!name_end) {
            break;
        }

        /* Parameters belong to this invoke, so stop at its closing tag. */
        inv_end = strstr(name_end, INVOKE_END);
        if (!inv_end) {
            break;
        }

        call = &resp->calls[resp->call_count];

        {
            size_t name_len = (size_t)(name_end - (inv + strlen(INVOKE)));

            if (name_len >= sizeof(call->name)) {
                name_len = sizeof(call->name) - 1;
            }
            memcpy(call->name, inv + strlen(INVOKE), name_len);
            call->name[name_len] = '\0';
        }

        snprintf(call->id, sizeof(call->id), "dsml_%d", resp->call_count);

        args = cJSON_CreateObject();
        scan = name_end;

        while (scan < inv_end) {
            const char* ps = strstr(scan, PARAM);
            const char* key_end;
            const char* val;
            const char* val_end;

            if (!ps || ps >= inv_end) {
                break;
            }

            key_end = strchr(ps + strlen(PARAM), '"');
            if (!key_end || key_end > inv_end) {
                break;
            }

            val = key_end + 1;
            if (*val == '>') {
                val++;
            }

            val_end = strchr(val, '<');
            if (!val_end || val_end > inv_end) {
                break;
            }

            {
                char key[64];
                size_t klen = (size_t)(key_end - (ps + strlen(PARAM)));
                char* value;

                if (klen >= sizeof(key)) {
                    klen = sizeof(key) - 1;
                }
                memcpy(key, ps + strlen(PARAM), klen);
                key[klen] = '\0';

                value = calloc(1, (size_t)(val_end - val) + 1);
                if (value) {
                    memcpy(value, val, (size_t)(val_end - val));
                    cJSON_AddStringToObject(args, key, value);
                    free(value);
                }
            }

            scan = val_end;
        }

        {
            char* args_str = cJSON_PrintUnformatted(args);

            cJSON_Delete(args);
            if (args_str) {
                call->input = args_str;
                call->input_len = strlen(args_str);
            }
        }

        resp->call_count++;
        p = name_end + 1;
    }

    if (resp->call_count > 0) {
        resp->tool_use = true;
        free(resp->text);
        resp->text = NULL;
        resp->text_len = 0;
        syslog(LOG_WARNING,
            "[%s] Parsed %d tool call(s) from DSML\n",
            TAG, resp->call_count);
    }
}


/* ── Extract OpenAI tool_calls from response message ──────── */

void extract_openai_tool_calls(cJSON* message, llm_response_t* resp)
{
    cJSON* text_content = cJSON_GetObjectItem(message, "content");

    if (text_content && cJSON_IsString(text_content)
        && text_content->valuestring) {
        size_t tlen = strlen(text_content->valuestring);

        resp->text = calloc(1, tlen + 1);
        if (resp->text) {
            memcpy(resp->text, text_content->valuestring, tlen);
            resp->text_len = tlen;
        }
    }

    cJSON* rc = cJSON_GetObjectItem(message, "reasoning_content");

    if (rc && cJSON_IsString(rc) && rc->valuestring
        && rc->valuestring[0]) {
        resp->reasoning_content = strdup(rc->valuestring);
    }

    cJSON* tool_calls = cJSON_GetObjectItem(message, "tool_calls");

    if (!tool_calls || !cJSON_IsArray(tool_calls)) {
        return;
    }

    cJSON* tc;

    cJSON_ArrayForEach(tc, tool_calls)
    {
        if (resp->call_count >= AGENT_MAX_TOOL_CALLS) {
            break;
        }

        llm_tool_call_t* call = &resp->calls[resp->call_count];
        cJSON* id_item = cJSON_GetObjectItem(tc, "id");

        if (id_item && cJSON_IsString(id_item)) {
            strncpy(call->id, id_item->valuestring,
                sizeof(call->id) - 1);
        }

        cJSON* func_obj = cJSON_GetObjectItem(tc, "function");

        if (func_obj) {
            cJSON* n = cJSON_GetObjectItem(func_obj, "name");
            cJSON* a = cJSON_GetObjectItem(func_obj, "arguments");

            if (n && cJSON_IsString(n)) {
                strncpy(call->name, n->valuestring,
                    sizeof(call->name) - 1);
            }
            if (a && cJSON_IsString(a)) {
                size_t alen = strlen(a->valuestring);

                call->input = calloc(1, alen + 1);
                if (call->input) {
                    memcpy(call->input, a->valuestring, alen);
                    call->input_len = alen;
                }
            }
        }

        resp->call_count++;
    }

    if (resp->call_count > 0) {
        resp->tool_use = true;
    }
}

/* ── Response cleanup ─────────────────────────────────────── */

void llm_response_free(llm_response_t* resp)
{
    free(resp->text);
    resp->text = NULL;
    resp->text_len = 0;
    free(resp->reasoning_content);
    resp->reasoning_content = NULL;
    for (int i = 0; i < resp->call_count; i++) {
        free(resp->calls[i].input);
        resp->calls[i].input = NULL;
    }
    resp->call_count = 0;
    resp->tool_use = false;
}
