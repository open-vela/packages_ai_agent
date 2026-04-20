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

#include "tools/tool_fetch_url.h"
#include "agent_config.h"
#include "agent_compat.h"
#include "infra/url_parse.h"
#include "infra/vela_tls.h"
#include "infra/http_proxy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include "cJSON.h"

static const char *TAG = "tool_fetch";

#define FETCH_RESP_SIZE    (32 * 1024)
#define BINARY_CHECK_BYTES 512

/* Scan the first `len` bytes for non-text content.
 * Returns true if the buffer looks like binary (contains bytes that
 * are neither printable ASCII, TAB, CR, nor LF, and are not valid
 * UTF-8 lead/continuation bytes in a well-formed sequence). */
static bool is_binary_content(const char *buf, size_t len)
{
    size_t check = (len < BINARY_CHECK_BYTES) ? len : BINARY_CHECK_BYTES;

    for (size_t i = 0; i < check; i++) {
        unsigned char c = (unsigned char)buf[i];
        if (c == '\t' || c == '\n' || c == '\r') {
            continue;
        }
        if (c >= 0x20 && c <= 0x7E) {
            continue; /* printable ASCII */
        }
        if (c >= 0xC0 && c <= 0xF7) {
            continue; /* UTF-8 lead byte */
        }
        if (c >= 0x80 && c <= 0xBF) {
            continue; /* UTF-8 continuation byte */
        }
        /* Control char or invalid byte — likely binary */
        return true;
    }
    return false;
}

/* Block requests to private/loopback/link-local addresses (SSRF protection) */
static bool is_private_host(const char *host)
{
    /* Block obvious loopback names */
    if (strcmp(host, "localhost") == 0) return true;

    /* Try to parse as IPv4 */
    struct in_addr addr;
    if (inet_pton(AF_INET, host, &addr) == 1) {
        uint32_t ip = ntohl(addr.s_addr);
        /* Loopback */
        if ((ip >> 24) == 127) return true;
        /* RFC 1918 Class A */
        if ((ip >> 24) == 10) return true;
        /* RFC 1918 Class B */
        if ((ip >> 20) == (172 << 4 | 1)) return true;
        if ((ip & 0xFFF00000) == 0xAC100000) return true;
        /* RFC 1918 Class C */
        if ((ip >> 16) == (192 << 8 | 168)) return true;
        if ((ip & 0xFFFF0000) == 0xC0A80000) return true;
        /* Link-local */
        if ((ip >> 16) == (169 << 8 | 254)) return true;
        if ((ip & 0xFFFF0000) == 0xA9FE0000) return true;
        /* Unspecified */
        if (ip == 0) return true;
    }
    return false;
}

int tool_fetch_url_execute(const char *input_json, char *output, size_t output_size)
{
    cJSON *root = cJSON_Parse(input_json);
    if (!root) {
        snprintf(output, output_size, "Error: invalid JSON input");
        return ERROR;
    }

    const char *url = cJSON_GetStringValue(cJSON_GetObjectItem(root, "url"));
    if (!url) {
        snprintf(output, output_size, "Error: missing 'url' field");
        cJSON_Delete(root);
        return ERROR;
    }

    /* Parse URL */
    parsed_url_t pu;
    if (url_parse(url, &pu) != 0 || !pu.use_tls
        || strncmp(url, "https://", 8) != 0) {
        snprintf(output, output_size, "Error: only https:// URLs supported");
        cJSON_Delete(root);
        return ERROR;
    }

    /* SSRF protection: block private/loopback addresses */
    if (is_private_host(pu.host)) {
        syslog(LOG_WARNING, "[%s] Blocked SSRF attempt to private host: %s\n", TAG, pu.host);
        snprintf(output, output_size, "Error: access to private/internal addresses is not allowed");
        cJSON_Delete(root);
        return ERROR;
    }

    syslog(LOG_INFO, "[%s] Fetching: %s (host=%s port=%s)\n", TAG, url, pu.host, pu.port);

    char *resp = malloc(FETCH_RESP_SIZE);
    if (!resp) {
        snprintf(output, output_size, "Error: out of memory");
        cJSON_Delete(root);
        return ERROR;
    }

    int status = vela_https_get(pu.host, pu.port, pu.path, resp, FETCH_RESP_SIZE);

    if (status != 200) {
        snprintf(output, output_size, "Error: HTTP %d from %s", status, pu.host);
        free(resp);
        cJSON_Delete(root);
        return ERROR;
    }

    size_t resp_len = strlen(resp);

    /* Reject binary content — non-UTF-8 bytes break the JSON body
     * sent to the LLM API (observed: fetching .tar.gz returns gzip
     * magic bytes which cause "400: error parsing body"). */
    if (resp_len < 4 || is_binary_content(resp, resp_len)) {
        syslog(LOG_WARNING, "[%s] Binary content detected (%d bytes), "
               "returning placeholder\n", TAG, (int)resp_len);
        snprintf(output, output_size,
                 "[binary data: %d bytes from %s, "
                 "not displayable as text]", (int)resp_len, pu.host);
        free(resp);
        cJSON_Delete(root);
        return OK;
    }

    /* Truncate to fit output buffer */
    if (resp_len >= output_size) {
        resp_len = output_size - 1;
    }
    memcpy(output, resp, resp_len);
    output[resp_len] = '\0';

    syslog(LOG_INFO, "[%s] Fetched %d bytes from %s\n", TAG, (int)resp_len, pu.host);
    free(resp);
    cJSON_Delete(root);
    return OK;
}
