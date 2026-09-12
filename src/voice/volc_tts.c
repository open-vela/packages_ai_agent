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

#include "voice/volc_tts.h"
#include "infra/config_store.h"
#include "infra/vela_tls.h"
#include "agent_compat.h"
#include "agent_config.h"
#include "voice/voice_tts.h"

#include "cJSON.h"
#include "mbedtls/base64.h"
#include <stdlib.h>
#include <string.h>

static const char* TAG = "volc_tts";

/* Credentials loaded from config store */
static char s_api_key[128];
static char s_speaker[64];

static int volc_tts_init(void)
{
    memset(s_api_key, 0, sizeof(s_api_key));
    memset(s_speaker, 0, sizeof(s_speaker));

    claw_config_get(AGENT_CFG_KEY_VOLC_API_KEY,
        s_api_key, sizeof(s_api_key));

    if (claw_config_get(AGENT_CFG_KEY_VOLC_SPEAKER,
            s_speaker, sizeof(s_speaker))
            != OK
        || s_speaker[0] == '\0') {
        strncpy(s_speaker, AGENT_VOICE_DEFAULT_SPEAKER,
            sizeof(s_speaker) - 1);
    }

    return 0;
}

static char* build_tts_request(const char* text,
    const char* speaker)
{
    cJSON* root = cJSON_CreateObject();

    if (!root) {
        return NULL;
    }

    cJSON* params = cJSON_CreateObject();

    if (!params) {
        cJSON_Delete(root);
        return NULL;
    }

    cJSON_AddStringToObject(params, "text", text);
    cJSON_AddStringToObject(params, "speaker", speaker);

    cJSON* audio = cJSON_CreateObject();

    if (audio) {
        cJSON_AddStringToObject(audio, "format", "pcm");
        cJSON_AddNumberToObject(audio, "sample_rate",
            AGENT_VOICE_SAMPLE_RATE);
        cJSON_AddItemToObject(params, "audio_params", audio);
    }

    cJSON_AddItemToObject(root, "req_params", params);

    char* json_str = cJSON_PrintUnformatted(root);

    cJSON_Delete(root);
    return json_str;
}

static int volc_tts_synthesize(const char* text,
    unsigned char* pcm_out,
    size_t pcm_cap,
    size_t* pcm_len)
{
    if (!text || !pcm_out || !pcm_len) {
        return -EINVAL;
    }

    /* Reload credentials in case they changed at runtime */
    volc_tts_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] API key not configured\n", TAG);
        return -ENOENT;
    }

    *pcm_len = 0;

    char* body = build_tts_request(text, s_speaker);

    if (!body) {
        syslog(LOG_ERR, "[%s] Failed to build TTS request\n", TAG);
        return -ENOMEM;
    }

    vela_header_t hdrs[] = {
        { "x-api-key", s_api_key },
        { "X-Api-Resource-Id", AGENT_DOUBAO_TTS_RESOURCE },
        { "Connection", "close" },
        { NULL, NULL }
    };

    char* resp = calloc(1, AGENT_VOICE_RESP_BUF_SIZE);

    if (!resp) {
        free(body);
        return -ENOMEM;
    }

    syslog(LOG_INFO, "[%s] TTS V3 request: text=%zu bytes\n",
        TAG, strlen(text));

    size_t body_len;
    int status = vela_https_request(
        AGENT_DOUBAO_TTS_HOST, AGENT_DOUBAO_TTS_PORT,
        "POST", AGENT_DOUBAO_TTS_V3_PATH, hdrs,
        body, strlen(body),
        resp, AGENT_VOICE_RESP_BUF_SIZE, &body_len);

    free(body);
    body = NULL;

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

    size_t total_pcm = 0;
    int chunks = 0;
    char* line = resp;

    while (line && *line) {
        while (*line == '\n' || *line == '\r' || *line == ' ') {
            line++;
        }

        if (*line == '\0') {
            break;
        }

        char* eol = strchr(line, '\n');

        if (eol) {
            *eol = '\0';
        }

        cJSON* obj = cJSON_Parse(line);

        if (!obj) {
            /* Truncated JSON at buffer boundary — skip silently
             * if we already decoded some audio data. */
            if (total_pcm > 0) {
                syslog(LOG_WARNING,
                    "[%s] TTS: truncated chunk %d (buffer full), "
                    "using %zu bytes decoded so far\n",
                    TAG, chunks, total_pcm);
                break;
            }
            syslog(LOG_ERR, "[%s] TTS: bad JSON chunk %d: %.80s\n",
                TAG, chunks, line);
            break;
        }

        cJSON* code_j = cJSON_GetObjectItem(obj, "code");
        int code_val = code_j ? (int)code_j->valuedouble : -1;

        if (!code_j || !cJSON_IsNumber(code_j)
            || (code_val != 0 && code_val != 20000000)) {
            cJSON* msg_j = cJSON_GetObjectItem(obj, "message");

            syslog(LOG_ERR, "[%s] TTS error %d: %.80s\n",
                TAG, code_val,
                (msg_j && msg_j->valuestring)
                    ? msg_j->valuestring
                    : line);
            cJSON_Delete(obj);
            free(resp);
            return -EIO;
        }

        cJSON* data_j = cJSON_GetObjectItem(obj, "data");

        if (data_j && cJSON_IsString(data_j)
            && data_j->valuestring[0]) {
            const char* b64 = data_j->valuestring;
            size_t b64_len = strlen(b64);
            size_t decoded_len;

            if (total_pcm + (b64_len * 3 / 4) > pcm_cap) {
                syslog(LOG_ERR,
                    "[%s] TTS: PCM buffer full at chunk %d\n",
                    TAG, chunks);
                cJSON_Delete(obj);
                break;
            }

            mbedtls_base64_decode(
                pcm_out + total_pcm, pcm_cap - total_pcm,
                &decoded_len,
                (const unsigned char*)b64, b64_len);
            total_pcm += decoded_len;
            chunks++;
        }

        cJSON_Delete(obj);
        line = eol ? eol + 1 : NULL;
    }

    free(resp);
    resp = NULL;

    if (total_pcm == 0) {
        syslog(LOG_ERR, "[%s] TTS: no PCM data decoded\n", TAG);
        return -EPROTO;
    }

    *pcm_len = total_pcm;

    syslog(LOG_INFO,
        "[%s] TTS: synthesized %zu PCM bytes (%d chunks)\n",
        TAG, *pcm_len, chunks);
    return 0;
}

/* ── Streaming TTS (callback per chunk) ──────────────────────── */

/* Streaming path uses smaller b64 decode buffer than batch.
 * The resp buffer stays at 128KB to support up to ~4s of TTS
 * audio (typical LLM reply length).  For >4s audio, true
 * chunked HTTP reception would be needed (future work).
 * Net saving vs batch: 80KB (b64 96KB→16KB). */
#define VOLC_TTS_STREAM_RESP_SIZE (512 * 1024)
#define VOLC_TTS_STREAM_B64_SIZE (16 * 1024)

int volc_tts_synthesize_stream(const char* text,
    volc_tts_chunk_cb cb,
    void* user_data)
{
    if (!text || !cb) {
        return -EINVAL;
    }

    volc_tts_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] stream: API key not configured\n",
            TAG);
        return -ENOENT;
    }

    char* body = build_tts_request(text, s_speaker);

    if (!body) {
        return -ENOMEM;
    }

    vela_header_t hdrs[] = {
        { "x-api-key", s_api_key },
        { "X-Api-Resource-Id", AGENT_DOUBAO_TTS_RESOURCE },
        { "Connection", "close" },
        { NULL, NULL }
    };

    char* resp = calloc(1, VOLC_TTS_STREAM_RESP_SIZE);

    if (!resp) {
        free(body);
        return -ENOMEM;
    }

    size_t body_len;
    int status = vela_https_request(
        AGENT_DOUBAO_TTS_HOST, AGENT_DOUBAO_TTS_PORT,
        "POST", AGENT_DOUBAO_TTS_V3_PATH, hdrs,
        body, strlen(body),
        resp, VOLC_TTS_STREAM_RESP_SIZE, &body_len);

    free(body);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] stream: HTTP %d\n", TAG, status);
        free(resp);
        return -EIO;
    }

    if (body_len == 0) {
        free(resp);
        return -EPROTO;
    }

    /* Decode buffer for base64 chunks */
    unsigned char* pcm_tmp = malloc(VOLC_TTS_STREAM_B64_SIZE);

    if (!pcm_tmp) {
        free(resp);
        return -ENOMEM;
    }

    int chunks = 0;
    int err = 0;
    char* line = resp;

    while (line && *line) {
        while (*line == '\n' || *line == '\r' || *line == ' ') {
            line++;
        }

        if (*line == '\0') {
            break;
        }

        char* eol = strchr(line, '\n');

        if (eol) {
            *eol = '\0';
        }

        cJSON* obj = cJSON_Parse(line);

        if (!obj) {
            if (chunks > 0) {
                syslog(LOG_WARNING,
                    "[%s] stream: response truncated after "
                    "%d chunks (buffer full?)\n",
                    TAG, chunks);
                break;
            }
            syslog(LOG_ERR, "[%s] stream: bad JSON: %.80s\n",
                TAG, line);
            err = -EPROTO;
            break;
        }

        cJSON* code_j = cJSON_GetObjectItem(obj, "code");
        int code_val = code_j ? (int)code_j->valuedouble : -1;

        if (!code_j || !cJSON_IsNumber(code_j)
            || (code_val != 0 && code_val != 20000000)) {
            syslog(LOG_ERR, "[%s] stream: TTS error %d\n",
                TAG, code_val);
            cJSON_Delete(obj);
            err = -EIO;
            break;
        }

        cJSON* data_j = cJSON_GetObjectItem(obj, "data");

        if (data_j && cJSON_IsString(data_j)
            && data_j->valuestring[0]) {
            const char* b64 = data_j->valuestring;
            size_t b64_len = strlen(b64);
            size_t decoded_len;

            mbedtls_base64_decode(
                pcm_tmp, VOLC_TTS_STREAM_B64_SIZE,
                &decoded_len,
                (const unsigned char*)b64, b64_len);

            cb(pcm_tmp, decoded_len, 0, user_data);
            chunks++;
        }

        cJSON_Delete(obj);
        line = eol ? eol + 1 : NULL;
    }

    /* Send exactly one is_last=1 notification */
    if (chunks > 0 && err == 0) {
        cb(NULL, 0, 1, user_data);
    }

    free(pcm_tmp);
    free(resp);

    if (chunks == 0 && err == 0) {
        err = -EPROTO;
    }

    syslog(LOG_INFO, "[%s] stream: %d chunks delivered\n",
        TAG, chunks);
    return err;
}

/* Legacy API kept for backward compatibility */
int volc_tts_synthesize_compat(const char* text,
    const char* appid,
    const char* token,
    const char* speaker,
    unsigned char* pcm_out,
    size_t pcm_cap,
    size_t* pcm_len)
{
    (void)appid;
    (void)speaker;

    /* Temporarily override api_key if caller provides one */
    if (token && token[0] != '\0') {
        strncpy(s_api_key, token, sizeof(s_api_key) - 1);
        s_api_key[sizeof(s_api_key) - 1] = '\0';
    }

    return volc_tts_synthesize(text, pcm_out, pcm_cap, pcm_len);
}

/* Backend ops registration */

static int volc_tts_synthesize_stream_adapt(const char* text,
    voice_tts_chunk_cb cb,
    void* user_data)
{
    return volc_tts_ws_synthesize_stream(text,
        (volc_tts_chunk_cb)cb, user_data);
}

static const voice_tts_ops_t s_volc_tts_ops = {
    .name = "volcengine",
    .init = volc_tts_init,
    .synthesize = volc_tts_synthesize,
    .synthesize_stream = volc_tts_synthesize_stream_adapt,
    .deinit = NULL,
};

int volc_tts_register(void)
{
    return voice_tts_register(&s_volc_tts_ops);
}
