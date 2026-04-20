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

#ifndef __URL_PARSE_H__
#define __URL_PARSE_H__

#include <stdbool.h>

/**
 * url_parse.h — Shared URL parser for http(s) and ws(s) schemes.
 *
 * Supports: http:// https:// ws:// wss://
 * Extracts: host, port, path, use_tls flag.
 */

typedef struct {
    char host[128];
    char port[8];
    char path[256];
    bool use_tls;
} parsed_url_t;

/**
 * Parse a URL into host, port, path components.
 *
 * @param url   Full URL (e.g. "https://host:443/path")
 * @param out   Parsed result
 * @return 0 on success, -1 on unsupported scheme
 */
int url_parse(const char *url, parsed_url_t *out);

#endif /* __URL_PARSE_H__ */
