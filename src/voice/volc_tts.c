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
/* TTS 音质旋钮(1.0 = 引擎默认)。存成字符串,请求体里再 atof。 */
static char s_speed[16];
static char s_volume[16];
static char s_pitch[16];

/* Load one ratio knob; missing/empty falls back to the given default. */
static void load_ratio(const char *key, const char *fallback,
                       char *out, size_t cap)
{
    if (claw_config_get(key, out, cap) != OK || out[0] == '\0') {
        strncpy(out, fallback, cap - 1);
        out[cap - 1] = '\0';
    }
}

static int volc_tts_init(void)
{
    memset(s_api_key, 0, sizeof(s_api_key));
    memset(s_speaker, 0, sizeof(s_speaker));
    memset(s_speed, 0, sizeof(s_speed));
    memset(s_volume, 0, sizeof(s_volume));
    memset(s_pitch, 0, sizeof(s_pitch));

    claw_config_get(AGENT_CFG_KEY_VOLC_API_KEY,
        s_api_key, sizeof(s_api_key));

    if (claw_config_get(AGENT_CFG_KEY_VOLC_SPEAKER,
            s_speaker, sizeof(s_speaker))
            != OK
        || s_speaker[0] == '\0') {
        strncpy(s_speaker, AGENT_VOICE_DEFAULT_SPEAKER,
            sizeof(s_speaker) - 1);
    }

    load_ratio(AGENT_CFG_KEY_VOLC_SPEED, AGENT_VOICE_DEFAULT_SPEED,
               s_speed, sizeof(s_speed));
    load_ratio(AGENT_CFG_KEY_VOLC_VOLUME, AGENT_VOICE_DEFAULT_VOLUME,
               s_volume, sizeof(s_volume));
    load_ratio(AGENT_CFG_KEY_VOLC_PITCH, AGENT_VOICE_DEFAULT_PITCH,
               s_pitch, sizeof(s_pitch));

    return 0;
}

/* Re-read the three quality knobs from the config store.
 *
 * volc_tts_init() runs once when the backend is registered, so a runtime
 * `set_volc_voice` would otherwise stay invisible until the next boot — and
 * /data is tmpfs, so a reboot throws the new value away.  Callers that change
 * the knobs call this to make the next synthesis request use them. */
void volc_tts_reload_ratios(void)
{
    load_ratio(AGENT_CFG_KEY_VOLC_SPEED, AGENT_VOICE_DEFAULT_SPEED,
               s_speed, sizeof(s_speed));
    load_ratio(AGENT_CFG_KEY_VOLC_VOLUME, AGENT_VOICE_DEFAULT_VOLUME,
               s_volume, sizeof(s_volume));
    load_ratio(AGENT_CFG_KEY_VOLC_PITCH, AGENT_VOICE_DEFAULT_PITCH,
               s_pitch, sizeof(s_pitch));

    syslog(LOG_INFO, "[volc_tts] ratios reloaded: speed=%s volume=%s pitch=%s\n",
           s_speed, s_volume, s_pitch);
}

/* 把配置里的浮点比例(v1 语义,1.0=原速)换算成 v3 的整数 rate(-50..100)。
 *
 * v3(/api/v3/tts/unidirectional,res seed-tts-2.0)只认 speech_rate /
 * loudness_rate 两个整数;v1 的 speed_ratio/volume_ratio 浮点字段会被静默
 * 忽略——实测 set_volc_voice 0.9/0.7/0.5 三档语速听感完全一样。 */
static int tts_ratio_to_rate(const char *ratio)
{
    double r = atof(ratio);
    int v;

    if (r <= 0.0) {
        r = 1.0;
    }
    v = (int)((r - 1.0) * 100.0 + (r >= 1.0 ? 0.5 : -0.5));
    if (v < -50) {
        v = -50;
    }
    if (v > 100) {
        v = 100;
    }
    return v;
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
        /* v3 音质旋钮:整数 speech_rate / loudness_rate(-50..100,0=默认)。
         * 由配置里的 speed/volume 浮点比例换算,见 tts_ratio_to_rate()。
         * pitch 在 v3 seed-tts-2.0 上没有对应字段,仍发 v1 的 pitch_ratio
         * (被忽略,保留给未来切回 v1 的资源),想改音色请换 speaker。 */
        cJSON_AddNumberToObject(audio, "speech_rate",
            tts_ratio_to_rate(s_speed));
        cJSON_AddNumberToObject(audio, "loudness_rate",
            tts_ratio_to_rate(s_volume));
        if (strcmp(s_pitch, AGENT_VOICE_DEFAULT_RATIO) != 0) {
            cJSON_AddNumberToObject(audio, "pitch_ratio", atof(s_pitch));
        }
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

/* The body is consumed incrementally (vela_https_request_stream), so the
 * total response size is no longer capped by a fixed buffer: only the
 * largest single JSON line is held in memory, and the server emits ~48KB of
 * PCM (64KB of base64) per line.  The old code buffered the whole body in
 * 128KB, which cut every segment longer than ~2.5s of audio — at
 * speech_rate -50 that is barely half a sentence. */
#define VOLC_TTS_STREAM_B64_SIZE (72 * 1024)

typedef struct {
    unsigned char* pcm;
    size_t cap;
    volc_tts_chunk_cb cb;
    void* user;
    int chunks;
    int err;
} volc_tts_stream_ctx_t;

/* Called once per newline-delimited JSON record as it arrives off the wire. */
static int volc_tts_line_cb(const char* line, size_t len, void* user)
{
    volc_tts_stream_ctx_t* s = (volc_tts_stream_ctx_t*)user;

    if (len == 0) {
        return 0;
    }

    cJSON* obj = cJSON_Parse(line);

    if (!obj) {
        /* A single unparseable record must not kill the whole utterance. */
        syslog(LOG_WARNING, "[%s] stream: bad JSON line: %.40s\n", TAG, line);
        return 0;
    }

    cJSON* code_j = cJSON_GetObjectItem(obj, "code");
    int code_val = code_j ? (int)code_j->valuedouble : -1;

    if (!code_j || !cJSON_IsNumber(code_j)
        || (code_val != 0 && code_val != 20000000)) {
        syslog(LOG_ERR, "[%s] stream: TTS error %d\n", TAG, code_val);
        s->err = -EIO;
        cJSON_Delete(obj);
        return 1;  /* abort the transfer */
    }

    cJSON* data_j = cJSON_GetObjectItem(obj, "data");

    if (data_j && cJSON_IsString(data_j) && data_j->valuestring[0]) {
        const char* b64 = data_j->valuestring;
        size_t b64_len = strlen(b64);
        size_t decoded_len = 0;

        int b64rc = mbedtls_base64_decode(
            s->pcm, s->cap, &decoded_len,
            (const unsigned char*)b64, b64_len);

        /* Guard: on BUFFER_TOO_SMALL (or any error) mbedtls does NOT
         * write into the buffer but sets decoded_len to the required
         * size.  Using that stale length would read past the heap. */
        if (b64rc == 0 && decoded_len > 0 && decoded_len <= s->cap) {
            s->cb(s->pcm, decoded_len, 0, s->user);
            s->chunks++;
        } else {
            syslog(LOG_WARNING,
                "[%s] stream: base64 decode rc=%d need=%zu cap=%zu, "
                "skipping chunk\n",
                TAG, b64rc, decoded_len, s->cap);
        }
    }

    cJSON_Delete(obj);
    return 0;
}

int volc_tts_synthesize_stream(const char* text,
    volc_tts_chunk_cb cb,
    void* user_data)
{
    if (!text || !cb) {
        return -EINVAL;
    }

    volc_tts_init();

    if (s_api_key[0] == '\0') {
        syslog(LOG_ERR, "[%s] stream: API key not configured\n", TAG);
        return -ENOENT;
    }

    char* body = build_tts_request(text, s_speaker);

    if (!body) {
        return -ENOMEM;
    }

    /* No "Connection: close" here: the v3 unidirectional response is chunked,
     * so its end is self-delimiting and the TLS connection can go back to the
     * keep-alive pool.  A sentence-segmented reply then pays one TCP+TLS setup
     * for the whole utterance instead of one per segment -- on a lossy Wi-Fi
     * link a single setup can stall for tens of seconds and is heard as the
     * speaker dropping out between sentences. */
    vela_header_t hdrs[] = {
        { "x-api-key", s_api_key },
        { "X-Api-Resource-Id", AGENT_DOUBAO_TTS_RESOURCE },
        { NULL, NULL }
    };

    volc_tts_stream_ctx_t ctx;

    memset(&ctx, 0, sizeof(ctx));
    ctx.cap = VOLC_TTS_STREAM_B64_SIZE;
    ctx.cb = cb;
    ctx.user = user_data;
    ctx.pcm = malloc(ctx.cap);

    if (!ctx.pcm) {
        free(body);
        return -ENOMEM;
    }

    int status = vela_https_request_stream(
        AGENT_DOUBAO_TTS_HOST, AGENT_DOUBAO_TTS_PORT,
        "POST", AGENT_DOUBAO_TTS_V3_PATH, hdrs,
        body, strlen(body),
        volc_tts_line_cb, &ctx);

    free(body);
    free(ctx.pcm);

    if (status != 200) {
        syslog(LOG_ERR, "[%s] stream: HTTP %d\n", TAG, status);
        return -EIO;
    }

    if (ctx.err != 0) {
        return ctx.err;
    }

    if (ctx.chunks == 0) {
        syslog(LOG_ERR, "[%s] stream: no PCM chunks\n", TAG);
        return -EPROTO;
    }

    /* Send exactly one is_last=1 notification */
    cb(NULL, 0, 1, user_data);

    syslog(LOG_INFO, "[%s] stream: %d chunks delivered\n", TAG, ctx.chunks);
    return 0;
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
static int volc_tts_is_ready(void)
{
    /* Reload in case set_volc_key ran after registration */
    volc_tts_init();
    return s_api_key[0] != '\0';
}

static const voice_tts_ops_t s_volc_tts_ops = {
    .name = "volcengine",
    .init = volc_tts_init,
    .is_ready = volc_tts_is_ready,
    .synthesize = volc_tts_synthesize,
    .deinit = NULL,
};

int volc_tts_register(void)
{
    return voice_tts_register(&s_volc_tts_ops);
}
