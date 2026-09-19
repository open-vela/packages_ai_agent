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

/* mimo_asr.c - Xiaomi MiMo batch ASR backend (mimo-v2.5-asr).
 *
 * MiMo ASR rides the same OpenAI-compatible /v1/chat/completions endpoint as
 * the LLM and TTS. Audio is sent as a base64 data-url (input_audio) inside
 * messages[0].content; only WAV/MP3 are accepted, so the caller's raw
 * 16kHz/16-bit/mono PCM is wrapped in a 44-byte WAV header here. The
 * transcription comes back as plain text in choices[0].message.content.
 *
 * We reuse the LLM's api_key + llm_host config so no extra credentials are
 * needed (the Token Plan tp- key auths via "Authorization: Bearer", exactly
 * as mimo_tts.c does). */

#include "voice/mimo_asr.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_asr.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <syslog.h>

static const char* TAG = "mimo_asr";

/* MiMo ASR model and audio format. */
#define MIMO_ASR_MODEL    "mimo-v2.5-asr"
#define MIMO_ASR_LANGUAGE "zh"          /* kids speak Chinese ("auto"/"en" too) */
#define MIMO_ASR_MIME     "audio/wav"
#define MIMO_ASR_DATA_URL_PREFIX "data:audio/wav;base64,"

/* Default host for the Token Plan (tp-) key. Reuses llm_host when set. */
#define MIMO_ASR_DEFAULT_HOST "token-plan-cn.xiaomimimo.com"
#define MIMO_ASR_PATH "/v1/chat/completions"

/* ASR response is a small JSON object with the transcription text. */
#define MIMO_ASR_RESP_SIZE (16 * 1024)

/* PCM capture format the backend expects (matches the wake-loop capture). */
#define MIMO_ASR_SAMPLE_RATE 16000
#define MIMO_ASR_CHANNELS    1
#define MIMO_ASR_BITS        16

/* Credentials loaded from config store (shared with the MiMo LLM). */
static char s_api_key[128];
static char s_host[128];

static int mimo_asr_init(void)
{
    memset(s_api_key, 0, sizeof(s_api_key));
    memset(s_host, 0, sizeof(s_host));

    claw_config_get(AGENT_CFG_KEY_API_KEY, s_api_key, sizeof(s_api_key));

    if (claw_config_get(AGENT_CFG_KEY_LLM_HOST, s_host, sizeof(s_host))
            != OK
        || s_host[0] == '\0') {
        strncpy(s_host, MIMO_ASR_DEFAULT_HOST, sizeof(s_host) - 1);
    }

    return 0;
}

/* Little-endian writers for the WAV header. */
static void write_le16(unsigned char* p, uint16_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void write_le32(unsigned char* p, uint32_t v)
{
    p[0] = (unsigned char)(v & 0xff);
    p[1] = (unsigned char)((v >> 8) & 0xff);
    p[2] = (unsigned char)((v >> 16) & 0xff);
    p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* Build a canonical 44-byte RIFF/WAVE header for 16kHz/16-bit/mono PCM. */
static size_t build_wav_header(unsigned char* hdr, uint32_t data_len)
{
    uint32_t byte_rate = MIMO_ASR_SAMPLE_RATE * MIMO_ASR_CHANNELS
        * (MIMO_ASR_BITS / 8);

    memcpy(hdr + 0, "RIFF", 4);
    write_le32(hdr + 4, 36 + data_len);
    memcpy(hdr + 8, "WAVE", 4);

    memcpy(hdr + 12, "fmt ", 4);
    write_le32(hdr + 16, 16);                  /* fmt chunk size */
    write_le16(hdr + 20, 1);                   /* audio format = PCM */
    write_le16(hdr + 22, MIMO_ASR_CHANNELS);   /* channels */
    write_le32(hdr + 24, MIMO_ASR_SAMPLE_RATE);
    write_le32(hdr + 28, byte_rate);
    write_le16(hdr + 32, MIMO_ASR_CHANNELS * (MIMO_ASR_BITS / 8));
    write_le16(hdr + 34, MIMO_ASR_BITS);

    memcpy(hdr + 36, "data", 4);
    write_le32(hdr + 40, data_len);
    return 44;
}

static int mimo_asr_recognize(const unsigned char* pcm_data,
    size_t pcm_len,
    char* text_out,
    size_t text_cap)
{
    if (!pcm_data || pcm_len == 0 || !text_out || text_cap == 0) {
        return -EINVAL;
    }

    text_out[0] = '\0';

    mimo_asr_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] API key not configured (set 'api_key')\n",
            TAG);
        return -ENOENT;
    }

    /* ── 1. Wrap PCM in a WAV header ─────────────────────────── */
    size_t wav_len = 44 + pcm_len;
    unsigned char* wav = malloc(wav_len);
    if (!wav) {
        return -ENOMEM;
    }

    build_wav_header(wav, (uint32_t)pcm_len);
    memcpy(wav + 44, pcm_data, pcm_len);

    /* ── 2. Base64-encode the WAV ────────────────────────────── */
    size_t b64_cap = 4 * ((wav_len + 2) / 3) + 4;
    unsigned char* b64 = malloc(b64_cap);
    if (!b64) {
        free(wav);
        return -ENOMEM;
    }

    size_t b64_len = 0;
    int rc = mbedtls_base64_encode(b64, b64_cap, &b64_len, wav, wav_len);
    free(wav);

    if (rc != 0) {
        syslog(LOG_ERR, "[%s] base64 encode failed: %d\n", TAG, rc);
        free(b64);
        return -EFBIG;
    }

    /* ── 3. Build the data URL and JSON request ──────────────── */
    size_t prefix_len = strlen(MIMO_ASR_DATA_URL_PREFIX);
    char* data_url = malloc(prefix_len + b64_len + 1);
    if (!data_url) {
        free(b64);
        return -ENOMEM;
    }
    memcpy(data_url, MIMO_ASR_DATA_URL_PREFIX, prefix_len);
    memcpy(data_url + prefix_len, b64, b64_len);
    data_url[prefix_len + b64_len] = '\0';
    free(b64);

    cJSON* root = cJSON_CreateObject();
    cJSON* messages = cJSON_CreateArray();
    cJSON* msg = cJSON_CreateObject();
    cJSON* content = cJSON_CreateArray();
    cJSON* part = cJSON_CreateObject();
    cJSON* input_audio = cJSON_CreateObject();
    cJSON* asr_options = cJSON_CreateObject();

    if (!root || !messages || !msg || !content || !part
        || !input_audio || !asr_options) {
        free(data_url);
        if (root) cJSON_Delete(root);
        if (messages) cJSON_Delete(messages);
        if (msg) cJSON_Delete(msg);
        if (content) cJSON_Delete(content);
        if (part) cJSON_Delete(part);
        if (input_audio) cJSON_Delete(input_audio);
        if (asr_options) cJSON_Delete(asr_options);
        return -ENOMEM;
    }

    cJSON_AddStringToObject(root, "model", MIMO_ASR_MODEL);

    cJSON_AddStringToObject(msg, "role", "user");
    cJSON_AddStringToObject(part, "type", "input_audio");
    cJSON_AddStringToObject(input_audio, "data", data_url);
    cJSON_AddItemToObject(part, "input_audio", input_audio);
    cJSON_AddItemToArray(content, part);
    cJSON_AddItemToObject(msg, "content", content);
    cJSON_AddItemToArray(messages, msg);
    cJSON_AddItemToObject(root, "messages", messages);

    cJSON_AddStringToObject(asr_options, "language", MIMO_ASR_LANGUAGE);
    cJSON_AddItemToObject(root, "asr_options", asr_options);

    char* body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    free(data_url);

    if (!body) {
        return -ENOMEM;
    }

    /* ── 4. POST to chat/completions ─────────────────────────── */
    char auth[256];
    snprintf(auth, sizeof(auth), "Bearer %s", s_api_key);

    vela_header_t hdrs[] = {
        { "Authorization", auth },
        { "Content-Type", "application/json" },
        { NULL, NULL }
    };

    char* resp = calloc(1, MIMO_ASR_RESP_SIZE);
    if (!resp) {
        free(body);
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[%s] ASR request: pcm=%zu bytes host=%s\n",
        TAG, pcm_len, s_host);

    size_t resp_len = 0;
    int status = vela_https_request(
        s_host, "443", "POST", MIMO_ASR_PATH, hdrs,
        body, strlen(body),
        resp, MIMO_ASR_RESP_SIZE, &resp_len);

    free(body);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] ASR HTTP %d: %.256s\n",
            TAG, status, resp);
        free(resp);
        return -EIO;
    }

    /* ── 5. Parse choices[0].message.content ─────────────────── */
    cJSON* rroot = cJSON_Parse(resp);
    if (!rroot) {
        syslog(LOG_ERR, "[%s] ASR bad JSON: %.256s\n", TAG, resp);
        free(resp);
        return -EPROTO;
    }

    cJSON* choices = cJSON_GetObjectItem(rroot, "choices");
    cJSON* choice0 = (choices && cJSON_IsArray(choices))
        ? cJSON_GetArrayItem(choices, 0) : NULL;
    cJSON* rmsg = choice0 ? cJSON_GetObjectItem(choice0, "message") : NULL;
    cJSON* rcontent = rmsg ? cJSON_GetObjectItem(rmsg, "content") : NULL;

    int ret = 0;
    if (rcontent && cJSON_IsString(rcontent) && rcontent->valuestring) {
        strncpy(text_out, rcontent->valuestring, text_cap - 1);
        text_out[text_cap - 1] = '\0';
    } else {
        syslog(LOG_ERR, "[%s] ASR no text in response: %.256s\n",
            TAG, resp);
        ret = -EPROTO;
    }

    cJSON_Delete(rroot);
    free(resp);

    if (ret == 0) {
        syslog(LOG_INFO, "[%s] ASR text: \"%s\"\n", TAG, text_out);
    }

    return ret;
}

static void mimo_asr_deinit(void)
{
    memset(s_api_key, 0, sizeof(s_api_key));
    memset(s_host, 0, sizeof(s_host));
}

static const voice_asr_ops_t s_mimo_asr_ops = {
    .name = "mimo",
    .init = mimo_asr_init,
    .recognize = mimo_asr_recognize,
    .deinit = mimo_asr_deinit,
};

int mimo_asr_register(void)
{
    return voice_asr_register(&s_mimo_asr_ops);
}
