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

/* mimo_tts.c - Xiaomi MiMo TTS backend (mimo-v2.5-tts preset voices).
 *
 * MiMo TTS rides the OpenAI-compatible /v1/chat/completions endpoint.
 * We reuse the LLM's api_key + llm_host config so no extra credentials
 * are needed. Output is "pcm16" = 24kHz PCM16LE mono, which matches
 * AGENT_TTS_WS_SAMPLE_RATE used by voice_channel_speak() playback. */

#include "voice/mimo_tts.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_tts.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "mimo_tts";

/* MiMo preset voice (Chinese, female). */
#define MIMO_TTS_VOICE "冰糖"

/* MiMo TTS model and audio format. */
#define MIMO_TTS_MODEL "mimo-v2.5-tts"
#define MIMO_TTS_FORMAT "pcm16"

/* Default host for the Token Plan (tp-) key. Reuses llm_host when set. */
#define MIMO_TTS_DEFAULT_HOST "token-plan-cn.xiaomimimo.com"
#define MIMO_TTS_PATH "/v1/chat/completions"

/* Response buffer: holds the full base64 audio for one utterance.
 * ~384KB base64 ≈ 4s of 24kHz/16-bit/mono speech. A long story sentence
 * (~30 Chinese chars ≈ 7s ≈ 450KB base64) needs more; 1MB covers it with
 * headroom while sentences are kept short by the caller's chunking. */
#define MIMO_TTS_RESP_SIZE (1024 * 1024)

/* Base64 is decoded in segments so we never need a second full-size PCM
 * buffer. Segment size must stay a multiple of 4 to keep 3-byte groups
 * aligned across segment boundaries. */
#define MIMO_B64_SEG (16 * 1024)              /* 16384, multiple of 4 */
#define MIMO_PCM_SEG (MIMO_B64_SEG / 4 * 3)   /* 12288 bytes of PCM */

/* The MiMo edge (token-plan-cn.xiaomimimo.com, Xiaomi ALB) intermittently
 * drops/corrupts TLS data for minutes at a time ("bad window"). During such
 * a window the handshake can succeed yet the response still fails with
 * MBEDTLS_ERR_SSL_INVALID_MAC (-0x7180 → VELA_TLS_ERR_READ), so a single
 * request attempt isn't enough. Retry the whole request (fresh connection
 * each time); each attempt already carries its own 8-round handshake retry
 * inside tls_ctx_connect_retry(). */
#define MIMO_TTS_HTTP_ATTEMPTS   3
#define MIMO_TTS_RETRY_DELAY_MS  1500

/* Credentials loaded from config store (shared with the MiMo LLM). */
static char s_api_key[128];
static char s_host[128];

static int mimo_tts_init(void)
{
    memset(s_api_key, 0, sizeof(s_api_key));
    memset(s_host, 0, sizeof(s_host));

    claw_config_get(AGENT_CFG_KEY_API_KEY, s_api_key, sizeof(s_api_key));

    if (claw_config_get(AGENT_CFG_KEY_LLM_HOST, s_host, sizeof(s_host))
            != OK
        || s_host[0] == '\0') {
        strncpy(s_host, MIMO_TTS_DEFAULT_HOST, sizeof(s_host) - 1);
    }

    return 0;
}

static char* build_tts_request(const char* text)
{
    cJSON* root = cJSON_CreateObject();

    if (!root) {
        return NULL;
    }

    cJSON_AddStringToObject(root, "model", MIMO_TTS_MODEL);

    cJSON* messages = cJSON_CreateArray();
    cJSON* assistant = cJSON_CreateObject();

    if (!messages || !assistant) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddStringToObject(assistant, "role", "assistant");
    cJSON_AddStringToObject(assistant, "content", text);
    cJSON_AddItemToArray(messages, assistant);
    cJSON_AddItemToObject(root, "messages", messages);

    cJSON* audio = cJSON_CreateObject();

    if (!audio) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddStringToObject(audio, "format", MIMO_TTS_FORMAT);
    cJSON_AddStringToObject(audio, "voice", MIMO_TTS_VOICE);
    cJSON_AddItemToObject(root, "audio", audio);

    char* json_str = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);
    return json_str;
}

/* One fetched TTS response. resp holds the raw HTTP body; root holds the
 * parsed JSON (the base64 string lives inside root, not resp). */
typedef struct {
    char* resp;      /* owned, caller frees */
    cJSON* root;     /* owned, caller frees */
    const char* b64; /* pointer into root's strings */
    size_t b64_len;
} mimo_audio_t;

static void mimo_audio_free(mimo_audio_t* a)
{
    if (a->root) {
        cJSON_Delete(a->root);
    }
    free(a->resp);
    memset(a, 0, sizeof(*a));
}

static int mimo_fetch_audio(const char* text, mimo_audio_t* out)
{
    memset(out, 0, sizeof(*out));

    mimo_tts_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] API key not configured (set 'api_key')\n",
            TAG);
        return -ENOENT;
    }

    char* body = build_tts_request(text);

    if (!body) {
        syslog(LOG_ERR, "[%s] failed to build TTS request\n", TAG);
        return -ENOMEM;
    }

    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);

    /* NOTE: do NOT send "Connection: close". tls_write_request() already
     * emits "Connection: keep-alive", and a "close" header here would force
     * the server to drop the socket after every sentence. That made each
     * TTS sentence pay a fresh TCP+TLS handshake (~2-3s, or 10s+ on the
     * flaky MiMo edge), which is why a long story took ~50s to speak. */
    vela_header_t hdrs[] = {
        { "Authorization", auth },
        { "Content-Type", "application/json" },
        { NULL, NULL }
    };

    char* resp = calloc(1, MIMO_TTS_RESP_SIZE);

    if (!resp) {
        free(body);
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[%s] TTS request: text=%zu bytes host=%s\n",
        TAG, strlen(text), s_host);

    size_t body_len = 0;
    int status = 0;
    int attempt;

    for (attempt = 0; attempt < MIMO_TTS_HTTP_ATTEMPTS; attempt++) {
        status = vela_https_request(
            s_host, "443", "POST", MIMO_TTS_PATH, hdrs,
            body, strlen(body),
            resp, MIMO_TTS_RESP_SIZE, &body_len);

        if (status == 200) {
            break;
        }

        /* A negative status is a transport failure (handshake exhausted,
         * corrupted TLS record, read error) — transient on the flaky MiMo
         * edge, so retry the whole request on a fresh connection. A real
         * HTTP status (4xx/5xx) is not retried. */
        if (status >= 0) {
            break;
        }

        syslog(LOG_WARNING,
            "[%s] TTS HTTP %d (attempt %d/%d), retrying in %dms\n",
            TAG, status, attempt + 1, MIMO_TTS_HTTP_ATTEMPTS,
            MIMO_TTS_RETRY_DELAY_MS);

        usleep(MIMO_TTS_RETRY_DELAY_MS * 1000);
    }

    free(body);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] TTS HTTP %d: %.256s\n",
            TAG, status, resp);
        free(resp);
        return -EIO;
    }

    if (body_len == 0) {
        syslog(LOG_ERR, "[%s] TTS: empty response body\n", TAG);
        free(resp);
        return -EPROTO;
    }

    cJSON* root = cJSON_Parse(resp);

    if (!root) {
        syslog(LOG_ERR, "[%s] TTS: bad JSON: %.256s\n", TAG, resp);
        free(resp);
        return -EPROTO;
    }

    /* choices[0].message.audio.data */
    cJSON* choices = cJSON_GetObjectItem(root, "choices");
    cJSON* choice0 = (choices && cJSON_IsArray(choices))
        ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON* msg = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
    cJSON* audio = msg ? cJSON_GetObjectItem(msg, "audio") : NULL;
    cJSON* data = audio ? cJSON_GetObjectItem(audio, "data") : NULL;

    if (!data || !cJSON_IsString(data) || !data->valuestring[0]) {
        syslog(LOG_ERR, "[%s] TTS: no audio.data in response: %.256s\n",
            TAG, resp);
        cJSON_Delete(root);
        free(resp);
        return -EPROTO;
    }

    out->resp = resp;
    out->root = root;
    out->b64 = data->valuestring;
    out->b64_len = strlen(data->valuestring);
    return 0;
}

static int mimo_tts_synthesize(const char* text,
    unsigned char* pcm_out,
    size_t pcm_cap,
    size_t* pcm_len)
{
    if (!text || !pcm_out || !pcm_len) {
        return -EINVAL;
    }

    *pcm_len = 0;

    mimo_audio_t a;
    int ret = mimo_fetch_audio(text, &a);

    if (ret != 0) {
        return ret;
    }

    size_t olen = 0;
    int rc = mbedtls_base64_decode(pcm_out, pcm_cap, &olen,
        (const unsigned char*)a.b64, a.b64_len);

    if (rc != 0) {
        syslog(LOG_ERR, "[%s] TTS: base64 decode failed: %d\n", TAG, rc);
        mimo_audio_free(&a);
        return (rc == MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL)
            ? -EFBIG : -EPROTO;
    }

    *pcm_len = olen;
    mimo_audio_free(&a);

    syslog(LOG_INFO, "[%s] TTS: synthesized %zu PCM bytes\n", TAG, olen);
    return 0;
}

static int mimo_tts_synthesize_stream(const char* text,
    voice_tts_chunk_cb cb,
    void* user_data)
{
    if (!text || !cb) {
        return -EINVAL;
    }

    mimo_audio_t a;
    int ret = mimo_fetch_audio(text, &a);

    if (ret != 0) {
        return ret;
    }

    unsigned char* pcm_seg = malloc(MIMO_PCM_SEG + 4);

    if (!pcm_seg) {
        mimo_audio_free(&a);
        return -ENOMEM;
    }

    size_t off = 0;
    int chunks = 0;
    int err = 0;

    while (off < a.b64_len) {
        size_t chunk = a.b64_len - off;

        if (chunk > MIMO_B64_SEG) {
            chunk = MIMO_B64_SEG;
        }

        size_t olen = 0;
        int rc = mbedtls_base64_decode(pcm_seg, MIMO_PCM_SEG + 4, &olen,
            (const unsigned char*)(a.b64 + off), chunk);

        if (rc != 0) {
            syslog(LOG_ERR,
                "[%s] stream: base64 decode failed at %zu: %d\n",
                TAG, off, rc);
            err = -EPROTO;
            break;
        }

        if (olen > 0) {
            /* Halve playback volume: scale each 16-bit mono sample by
             * 0.5 (-6 dB). pcm_seg is our scratch buffer, so scale it in
             * place before handing it to the audio callback. */
            int16_t* s = (int16_t*)pcm_seg;
            size_t nsamp = olen / sizeof(int16_t);
            for (size_t i = 0; i < nsamp; i++) {
                s[i] = (int16_t)(s[i] / 2);
            }
            cb(pcm_seg, olen, 0, user_data);
            chunks++;
        }

        off += chunk;
    }

    if (chunks > 0 && err == 0) {
        cb(NULL, 0, 1, user_data);
    }

    if (chunks == 0 && err == 0) {
        err = -EPROTO;
    }

    free(pcm_seg);
    mimo_audio_free(&a);

    syslog(LOG_INFO, "[%s] stream: %d chunks delivered\n", TAG, chunks);
    return err;
}

static const voice_tts_ops_t s_mimo_tts_ops = {
    .name = "mimo",
    .init = mimo_tts_init,
    .synthesize = mimo_tts_synthesize,
    .synthesize_stream = mimo_tts_synthesize_stream,
    .deinit = NULL,
};

int mimo_tts_register(void)
{
    return voice_tts_register(&s_mimo_tts_ops);
}
