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

#pragma once

/**
 * http_body.h — HTTP/1.1 body end detection.
 *
 * The reader used to decide "body finished" only from Content-Length.  A
 * chunked reply carries no Content-Length, so on a keep-alive connection the
 * loop kept reading after the terminating zero-size chunk and waited for the
 * socket timeout instead of returning.  The same gap shows up as an inflated
 * body_ms in the timing line.
 *
 * HTTP/1.1 message length is determined by (RFC 9112 section 6.3), in order:
 *   1. Transfer-Encoding: chunked  -> ends at the zero-size chunk
 *   2. Content-Length              -> ends after exactly that many bytes
 *   3. Otherwise                   -> ends when the server closes the
 *      connection; a server that keeps it open cannot be distinguished from
 *      one still sending, so the socket timeout bounds it.
 *
 * These predicates are header-only so the host regression test exercises the
 * same code the firmware links.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Find CRLF at or after buf[pos].  Returns the index, or len when absent.
 * Written out rather than memmem() so the header stays portable to the host
 * test without _GNU_SOURCE.
 */
static inline size_t vela_http_find_crlf(const char* buf, size_t len,
    size_t pos)
{
    size_t i;

    for (i = pos; i + 1 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n') {
            return i;
        }
    }

    return len;
}

/**
 * True when the chunked body has reached its terminating zero-size chunk.
 *
 * Chunk sizes are parsed in order, so a "0" byte inside chunk data cannot be
 * mistaken for the terminator.  Trailer fields after the zero chunk are not
 * consumed; the body is complete as soon as the zero chunk header is seen.
 */
static inline bool vela_http_chunked_complete(const char* buf, size_t len)
{
    size_t pos = 0;

    while (pos < len) {
        size_t crlf = vela_http_find_crlf(buf, len, pos);
        char* endptr;
        long chunk_sz;

        if (crlf == len) {
            /* Size line not fully buffered yet. */

            return false;
        }

        chunk_sz = strtol(buf + pos, &endptr, 16);
        if (endptr != buf + crlf || chunk_sz < 0) {
            /* Malformed size line; let the read loop end by other means. */

            return false;
        }

        if (chunk_sz == 0) {
            return true;
        }

        pos = crlf + 2;

        if ((size_t)chunk_sz > len - pos) {
            /* Chunk data partially buffered. */

            return false;
        }

        pos += (size_t)chunk_sz;

        if (len - pos < 2) {
            /* Trailing CRLF after the chunk data not read yet. */

            return false;
        }

        if (buf[pos] != '\r' || buf[pos + 1] != '\n') {
            return false;
        }

        pos += 2;
    }

    return false;
}

/**
 * True when the response body is complete for framing purposes.
 *
 * @param chunked         Transfer-Encoding: chunked was present
 * @param content_length  Parsed Content-Length, or -1 when absent
 * @param resp_pos        Body bytes buffered so far, chunk framing included
 * @param buf             Body buffer, starting at the first body byte
 */
static inline bool vela_http_body_complete(bool chunked, long content_length,
    size_t resp_pos, const char* buf)
{
    if (chunked) {
        return vela_http_chunked_complete(buf, resp_pos);
    }

    if (content_length >= 0) {
        return resp_pos >= (size_t)content_length;
    }

    /* No framing: the peer closes the connection to end the body. */

    return false;
}

/**
 * True when the response carries no length information at all.  Such a reply
 * is only well defined on a connection that will be closed, so the caller
 * should say so in the log rather than leave the wait unattributed.
 */
static inline bool vela_http_body_unframed(bool chunked, long content_length)
{
    return !chunked && content_length < 0;
}

#ifdef __cplusplus
}
#endif
