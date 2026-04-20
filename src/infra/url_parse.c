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

#include "infra/url_parse.h"

#include <string.h>

int url_parse(const char *url, parsed_url_t *out)
{
    if (!url || !out)
        return -1;

    memset(out, 0, sizeof(*out));

    /* Strip scheme and set defaults */
    if (strncmp(url, "https://", 8) == 0) {
        out->use_tls = true;
        url += 8;
        strncpy(out->port, "443", sizeof(out->port) - 1);
    } else if (strncmp(url, "http://", 7) == 0) {
        out->use_tls = false;
        url += 7;
        strncpy(out->port, "80", sizeof(out->port) - 1);
    } else if (strncmp(url, "wss://", 6) == 0) {
        out->use_tls = true;
        url += 6;
        strncpy(out->port, "443", sizeof(out->port) - 1);
    } else if (strncmp(url, "ws://", 5) == 0) {
        out->use_tls = false;
        url += 5;
        strncpy(out->port, "80", sizeof(out->port) - 1);
    } else {
        return -1;
    }

    const char *slash = strchr(url, '/');
    const char *colon = strchr(url, ':');

    /* Extract host and optional port */
    if (colon && (!slash || colon < slash)) {
        size_t hlen = (size_t)(colon - url);
        if (hlen >= sizeof(out->host))
            hlen = sizeof(out->host) - 1;
        memcpy(out->host, url, hlen);
        out->host[hlen] = '\0';

        colon++;
        const char *pend = slash ? slash : colon + strlen(colon);
        size_t plen = (size_t)(pend - colon);
        if (plen >= sizeof(out->port))
            plen = sizeof(out->port) - 1;
        memcpy(out->port, colon, plen);
        out->port[plen] = '\0';
    } else {
        size_t hlen = slash ? (size_t)(slash - url) : strlen(url);
        if (hlen >= sizeof(out->host))
            hlen = sizeof(out->host) - 1;
        memcpy(out->host, url, hlen);
        out->host[hlen] = '\0';
    }

    /* Extract path */
    if (slash) {
        strncpy(out->path, slash, sizeof(out->path) - 1);
        out->path[sizeof(out->path) - 1] = '\0';
    } else {
        strncpy(out->path, "/", sizeof(out->path) - 1);
    }

    /* Reject empty host */
    if (out->host[0] == '\0')
        return -1;

    return 0;
}
