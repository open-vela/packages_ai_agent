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

/**
 * llm_vision.c — Vision chat (text + image) for AI Agent.
 *
 * Extracted from llm_proxy.c:
 *   - llm_chat_vision()      — base64 image input
 *   - llm_chat_vision_raw()  — raw bytes input (memory-optimized)
 *   - json_escape_string()   — helper for safe JSON embedding
 */

#include "llm/llm_internal.h"
#include "llm/llm_proxy.h"
#include "agent_compat.h"
#include "agent_config.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

static const char* TAG = "llm_vision";

/* ── Public: vision chat (text + image) ───────────────────── */

int llm_chat_vision(const char* prompt, const char* image_b64,
    const char* mime_type, char* response_buf,
    size_t buf_size)
{
    char model[64], api_key[128], llm_host[128];
    llm_snapshot_vision_config(model, sizeof(model),
        api_key, sizeof(api_key),
        llm_host, sizeof(llm_host));

    if (api_key[0] == '\0') {
        snprintf(response_buf, buf_size, "Error: No API key configured");
        return ERROR;
    }

    if (!mime_type || !mime_type[0])
        mime_type = "image/jpeg";

    cJSON* content_arr = cJSON_CreateArray();
    cJSON* text_part = cJSON_CreateObject();
    cJSON* img_part = cJSON_CreateObject();
    cJSON* img_url_obj = cJSON_CreateObject();
    cJSON* user_msg = cJSON_CreateObject();
    cJSON* messages = cJSON_CreateArray();
    cJSON* body = cJSON_CreateObject();
    char* data_uri = NULL;

    if (!content_arr || !text_part || !img_part || !img_url_obj
        || !user_msg || !messages || !body) {
        cJSON_Delete(content_arr);
        cJSON_Delete(text_part);
        cJSON_Delete(img_part);
        cJSON_Delete(img_url_obj);
        cJSON_Delete(user_msg);
        cJSON_Delete(messages);
        cJSON_Delete(body);
        snprintf(response_buf, buf_size, "Error: OOM building vision JSON");
        return ERROR;
    }

    cJSON_AddStringToObject(text_part, "type", "text");
    cJSON_AddStringToObject(text_part, "text",
        prompt ? prompt : AGENT_VISION_DEFAULT_PROMPT);
    cJSON_AddItemToArray(content_arr, text_part);

    size_t b64_len = strlen(image_b64);
    size_t prefix_len = 5 + strlen(mime_type) + 8;
    size_t uri_len = prefix_len + b64_len + 1;
    data_uri = malloc(uri_len);
    if (!data_uri) {
        cJSON_Delete(content_arr);
        cJSON_Delete(img_part);
        cJSON_Delete(img_url_obj);
        cJSON_Delete(user_msg);
        cJSON_Delete(messages);
        cJSON_Delete(body);
        snprintf(response_buf, buf_size, "Error: OOM building data URI");
        return ERROR;
    }
    snprintf(data_uri, uri_len, "data:%s;base64,%s", mime_type, image_b64);

    cJSON_AddStringToObject(img_part, "type", "image_url");
    cJSON_AddStringToObject(img_url_obj, "url", data_uri);
    cJSON_AddItemToObject(img_part, "image_url", img_url_obj);
    cJSON_AddItemToArray(content_arr, img_part);
    free(data_uri);

    cJSON_AddStringToObject(user_msg, "role", "user");
    cJSON_AddItemToObject(user_msg, "content", content_arr);

    cJSON_AddItemToArray(messages, user_msg);

    cJSON_AddStringToObject(body, "model",
        model_name_for_api(model, llm_host));

    if (is_openai_compat_host(llm_host))
        cJSON_AddNumberToObject(body, "max_completion_tokens",
            AGENT_VISION_MAX_TOKENS);
    else
        cJSON_AddNumberToObject(body, "max_tokens", AGENT_VISION_MAX_TOKENS);

    cJSON_AddItemToObject(body, "messages", messages);

    char* post_data = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    if (!post_data) {
        snprintf(response_buf, buf_size, "Error: Failed to build vision request");
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Vision API call (model: %s, %d bytes)\n", TAG, model,
        (int)strlen(post_data));

    resp_buf_t rb = { 0 };
    int status = 0;
    int err = llm_http_call(post_data, &rb, &status);
    free(post_data);

    if (err != OK) {
        resp_buf_free(&rb);
        snprintf(response_buf, buf_size, "Error: Vision HTTP request failed");
        return err;
    }

    if (status != 200) {
        syslog(LOG_ERR, "[%s] Vision API error HTTP %d: %.300s\n", TAG, status,
            rb.data ? rb.data : "");
        snprintf(response_buf, buf_size, "Vision API error (HTTP %d): %.200s",
            status, rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return ERROR;
    }

    cJSON* root = cJSON_Parse(rb.data);
    resp_buf_free(&rb);

    if (!root) {
        snprintf(response_buf, buf_size, "Error: Failed to parse vision response");
        return ERROR;
    }

    extract_text(root, response_buf, buf_size);
    cJSON_Delete(root);

    if (response_buf[0] == '\0')
        snprintf(response_buf, buf_size, "No response from Vision API");
    else
        syslog(LOG_INFO, "[%s] Vision response: %d bytes\n", TAG,
            (int)strlen(response_buf));

    return OK;
}

/* ── JSON string escape helper ────────────────────────────── */

static char* json_escape_string(const char* src)
{
    if (!src)
        return NULL;

    size_t src_len = strlen(src);
    size_t cap = src_len * 6 + 1;
    char* out = malloc(cap);
    if (!out)
        return NULL;

    char* w = out;
    for (const char* r = src; *r; r++) {
        unsigned char c = (unsigned char)*r;
        switch (c) {
        case '"':  *w++ = '\\'; *w++ = '"';  break;
        case '\\': *w++ = '\\'; *w++ = '\\'; break;
        case '\n': *w++ = '\\'; *w++ = 'n';  break;
        case '\r': *w++ = '\\'; *w++ = 'r';  break;
        case '\t': *w++ = '\\'; *w++ = 't';  break;
        case '\b': *w++ = '\\'; *w++ = 'b';  break;
        case '\f': *w++ = '\\'; *w++ = 'f';  break;
        default:
            if (c < 0x20) {
                w += sprintf(w, "\\u%04x", c);
            } else {
                *w++ = (char)c;
            }
            break;
        }
    }
    *w = '\0';
    return out;
}

/* ── Public: memory-optimized vision chat (raw image bytes) ── */

int llm_chat_vision_raw(const char* prompt,
    const unsigned char* raw_image, size_t raw_len,
    const char* mime_type,
    char* response_buf, size_t buf_size)
{
    char model[64], api_key[128], llm_host[128];
    llm_snapshot_vision_config(model, sizeof(model),
        api_key, sizeof(api_key),
        llm_host, sizeof(llm_host));

    if (api_key[0] == '\0') {
        snprintf(response_buf, buf_size, "Error: No API key configured");
        return ERROR;
    }

    if (!mime_type || !mime_type[0])
        mime_type = "image/jpeg";

    if (!prompt || !prompt[0])
        prompt = AGENT_VISION_DEFAULT_PROMPT;

    char* escaped_prompt = json_escape_string(prompt);
    if (!escaped_prompt) {
        snprintf(response_buf, buf_size, "Error: OOM escaping prompt");
        return ERROR;
    }

    int max_tokens = AGENT_VISION_MAX_TOKENS;
    const char* tokens_key = is_openai_compat_host(llm_host)
        ? "max_completion_tokens"
        : "max_tokens";

    size_t b64_len = 0;
    mbedtls_base64_encode(NULL, 0, &b64_len, raw_image, raw_len);

    const char* hdr_fmt = "{\"model\":\"%s\",\"%s\":%d,\"messages\":"
                          "[{\"role\":\"user\",\"content\":["
                          "{\"type\":\"text\",\"text\":\"%s\"},"
                          "{\"type\":\"image_url\",\"image_url\":"
                          "{\"url\":\"data:%s;base64,";
    const char* trailer = "\"}}]}]}";

    size_t hdr_max = strlen(hdr_fmt) + sizeof(model) + 32
        + strlen(tokens_key) + strlen(escaped_prompt)
        + strlen(mime_type) + 64;
    size_t total = hdr_max + b64_len + strlen(trailer) + 1;

    char* body = malloc(total);
    if (!body) {
        free(escaped_prompt);
        snprintf(response_buf, buf_size, "Error: OOM building vision request");
        return ERROR;
    }

    int off = snprintf(body, total, hdr_fmt,
        model_name_for_api(model, llm_host), tokens_key, max_tokens,
        escaped_prompt, mime_type);
    free(escaped_prompt);

    size_t written = 0;
    int rc = mbedtls_base64_encode(
        (unsigned char*)(body + off),
        total - (size_t)off - strlen(trailer) - 1,
        &written, raw_image, raw_len);
    if (rc != 0) {
        free(body);
        snprintf(response_buf, buf_size,
            "Error: base64 encode failed (%d)", rc);
        return ERROR;
    }
    off += (int)written;

    memcpy(body + off, trailer, strlen(trailer) + 1);

    syslog(LOG_INFO,
        "[%s] Vision raw API call (model: %s, body=%d bytes, "
        "image=%zu raw -> %zu b64)\n",
        TAG, model, off + (int)strlen(trailer), raw_len, written);

    resp_buf_t rb = { 0 };
    int status = 0;
    int err = llm_http_call(body, &rb, &status);
    free(body);

    if (err != OK) {
        resp_buf_free(&rb);
        snprintf(response_buf, buf_size, "Error: Vision HTTP request failed");
        return err;
    }

    if (status != 200) {
        syslog(LOG_ERR, "[%s] Vision raw API error HTTP %d: %.300s\n",
            TAG, status, rb.data ? rb.data : "");
        snprintf(response_buf, buf_size,
            "Vision API error (HTTP %d): %.200s",
            status, rb.data ? rb.data : "");
        resp_buf_free(&rb);
        return ERROR;
    }

    cJSON* root = cJSON_Parse(rb.data);
    resp_buf_free(&rb);

    if (!root) {
        snprintf(response_buf, buf_size,
            "Error: Failed to parse vision response");
        return ERROR;
    }

    extract_text(root, response_buf, buf_size);
    cJSON_Delete(root);

    if (response_buf[0] == '\0')
        snprintf(response_buf, buf_size, "No response from Vision API");
    else
        syslog(LOG_DEBUG, "[%s] Vision response: %d bytes\n", TAG,
            (int)strlen(response_buf));

    return OK;
}
