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

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "voice/voice_tts.h" /* for voice_tts_chunk_cb */

/*
 * HTTP TTS backend: streams raw PCM (24kHz / mono / s16le) from a
 * self-hosted service so the device can speak without paid cloud TTS
 * credentials.  The agent issues
 *     GET <base_url>?text=<percent-encoded>&rate=24000
 * and the server returns a raw PCM body (audio/L16) which we feed to
 * the playback device chunk-by-chunk as it arrives.
 */

/* Load the base URL from config_store (key "tts_http_url"), falling
 * back to CONFIG_AI_AGENT_TTS_HTTP_URL.  Call once at init. */
int http_tts_init(void);

/* Override the TTS base URL at runtime (mainly for tests). */
void http_tts_set_url(const char *url);

/* Synthesize `text` via a streaming HTTP GET.  Each decoded PCM chunk
 * is delivered through `cb` as it arrives from the server.  Returns 0
 * on success, or a negative errno on transport / HTTP error so the
 * caller can fall back to another backend. */
/* [ONE-VOICE 2026-10-27] 探测 TTS 服务是否可达（只做 TCP connect，不产生请求）。
 * 返回 0 = 在线，负值 = 不可达（调用方应回退到服务端音频）。 */
int http_tts_ping(void);

int http_tts_speak_stream(const char *text,
                          voice_tts_chunk_cb cb,
                          void *user_data);

/* [TTS-ABORT 2026-10-28] 从另一线程放弃正在进行的 speak，立即关掉其 socket，
 * 让阻塞在 recv() 的 http_tts 线程即时退出、连接释放（修复被放弃句子一直占
 * 连接导致 LwIP 耗尽、新 TTS 收不到 body 而 EIO 静音）。 */
void http_tts_abort_active(void);

/* [V68] Post a short diagnostic tag to the TTS bridge's /diag endpoint.
 * Log-only on the server side, two-byte reply, so it is cheap enough to use
 * as a probe channel on a board with no console.  Returns 0 on success. */
int diag_report(const char *msg);

#ifdef __cplusplus
}
#endif
