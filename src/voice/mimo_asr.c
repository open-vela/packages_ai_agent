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

/****************************************************************************
 * Xiaomi MiMo ASR 后端 (mimo-v2.5-asr, Token Plan 网关)
 *
 * 协议(OpenAI 兼容 chat/completions, 主机的 curl 已实测):
 *   POST https://token-plan-cn.xiaomimimo.com/v1/chat/completions
 *   {
 *     "model": "mimo-v2.5-asr",
 *     "messages": [ { "role": "user", "content": [
 *         { "type": "input_audio",
 *           "input_audio": { "data": "data:audio/wav;base64,<...>" } } ] } ],
 *     "asr_options": { "language": "zh" }
 *   }
 * 识别结果在 choices[0].message.content。
 *
 * 与 minimax_asr.c 的差别:
 *   - MiMo 只收 wav/mp3, 不收裸 PCM/Ogg Opus; 设备端只有 MP3 **解码器**,
 *     没有编码器, 所以这里上传 **WAV**(16kHz/mono/16bit + 44B 头)。
 *   - 只上传 VAD 裁剪后的"有人声"帧, 一句话通常 1~3 秒(32~96KB PCM),
 *     base64 后 43~128KB; 蓝牙 PAN 上行约 11KB/s => 4~12 秒传完。
 *     为防长语音把延迟拖爆, 硬上限 8 秒人声。
 ****************************************************************************/

#include "voice/voice_asr.h"
#include "voice/vad_trim.h"
#include "voice/mimo_asr.h"
#include "voice/mimo_key_local.h"
#include "infra/vela_tls.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include <mbedtls/base64.h>

#include "cJSON.h"

#define MIMO_ASR_TAG       "mimo_asr"

#define MIMO_ASR_RESP_CAP  4096u
#define MIMO_ASR_MAX_PCM   (8u * 16000u * 2u)   /* 输入上限: 8 秒 16kHz/mono/16bit */
#define MIMO_ASR_RATE_OUT  8000u                /* 上传降到 8kHz: 文件减半, 慢网省一半上传时间 */
#define MIMO_ASR_WAV_HDR   44u

static char s_auth[128];
static int  s_ready;

static void mimo_asr_le16(unsigned char* p, unsigned v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void mimo_asr_le32(unsigned char* p, unsigned long v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* 44 字节标准 WAV 头: RIFF/WAVE + fmt(16, PCM, mono, rate, 16bit) + data */
static void mimo_asr_wav_header(unsigned char* h, unsigned long pcm_len,
                                unsigned long rate)
{
  memcpy(h, "RIFF", 4);
  mimo_asr_le32(h + 4, 36u + pcm_len);
  memcpy(h + 8, "WAVEfmt ", 8);
  mimo_asr_le32(h + 16, 16u);
  mimo_asr_le16(h + 20, 1u);              /* PCM */
  mimo_asr_le16(h + 22, 1u);              /* mono */
  mimo_asr_le32(h + 24, rate);
  mimo_asr_le32(h + 28, rate * 2u);       /* byte rate = rate * 16bit * 1ch */
  mimo_asr_le16(h + 32, 2u);              /* block align */
  mimo_asr_le16(h + 34, 16u);             /* bits per sample */
  memcpy(h + 36, "data", 4);
  mimo_asr_le32(h + 40, pcm_len);
}

/* 16kHz -> 8kHz 降采样: 每两个样点取平均(半带低通 + 2:1 抽取)。
 * 目的只是把上行 WAV 体积减半; 语音识别对 8kHz 的普通话足够。 */
static size_t mimo_asr_downsample(const unsigned char* in, size_t in_len,
                                  unsigned char* out)
{
  size_t pairs = in_len / 4u;            /* 每 4 字节 = 2 个 int16 样点 */
  const short* src = (const short*)(const void*)in;
  short*       dst = (short*)(void*)out;

  for (size_t i = 0; i < pairs; i++)
    {
      int a = (int)src[2u * i];
      int b = (int)src[2u * i + 1u];

      dst[i] = (short)((a + b) / 2);
    }

  return pairs * 2u;
}

static int mimo_asr_init(void)
{
  if (MIMO_API_KEY_LOCAL[0] == '\0')
    {
      syslog(LOG_ERR, "[%s] no build-time MiMo key\n", MIMO_ASR_TAG);
      return -ENOENT;
    }

  if (s_auth[0] == '\0')
    {
      snprintf(s_auth, sizeof(s_auth), "Bearer %s", MIMO_API_KEY_LOCAL);
    }

  s_ready = 1;
  syslog(LOG_INFO, "[%s] ready (model %s, host %s)\n",
         MIMO_ASR_TAG, MIMO_ASR_MODEL, MIMO_API_HOST);
  return 0;
}

/* 把 PCM 包成 WAV、base64、组 JSON、POST，解析回文字。
 * 批量与流式两条路径共用。 */
static int mimo_asr_transcribe(const unsigned char* pcm, size_t pcm_len,
                               char* text_out, size_t text_cap)
{
  unsigned char* wav = NULL;
  unsigned char* b64 = NULL;
  char*  body = NULL;
  char*  resp = NULL;
  cJSON* root = NULL;
  size_t b64_cap;
  size_t b64_len = 0;
  size_t body_cap;
  size_t wav_len = 0;
  vela_header_t hdrs[2];
  int    status;
  int    ret = -EIO;

  if (text_out == NULL || text_cap == 0)
    {
      return -EINVAL;
    }

  text_out[0] = '\0';

  if (!s_ready && mimo_asr_init() != 0)
    {
      return -ENOENT;
    }

  if (pcm == NULL || pcm_len == 0)
    {
      return -EINVAL;
    }

  if (pcm_len > MIMO_ASR_MAX_PCM)
    {
      pcm_len = MIMO_ASR_MAX_PCM;
    }

  {
    size_t ds_bytes;

    wav = (unsigned char*)malloc(MIMO_ASR_WAV_HDR + (pcm_len / 2u) + 4u);
    if (wav == NULL)
      {
        return -ENOMEM;
      }

    /* 16k -> 8k 降采样后再包 WAV: 上行体积直接减半。 */
    ds_bytes = mimo_asr_downsample(pcm, pcm_len, wav + MIMO_ASR_WAV_HDR);

    mimo_asr_wav_header(wav, (unsigned long)ds_bytes, MIMO_ASR_RATE_OUT);
    wav_len = MIMO_ASR_WAV_HDR + ds_bytes;

    b64_cap = 4u * ((wav_len + 2u) / 3u) + 4u;
    b64 = (unsigned char*)malloc(b64_cap);
    if (b64 == NULL)
      {
        free(wav);
        return -ENOMEM;
      }

    if (mbedtls_base64_encode(b64, b64_cap, &b64_len, wav, wav_len) != 0)
      {
        ret = -EPROTO;
        goto out;
      }
  }

  b64[b64_len] = '\0';

  root = cJSON_CreateObject();
  if (root == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  cJSON_AddStringToObject(root, "model", MIMO_ASR_MODEL);

  cJSON* msgs = cJSON_CreateArray();
  cJSON* msg  = cJSON_CreateObject();
  cJSON* cont = cJSON_CreateArray();
  cJSON* part = cJSON_CreateObject();
  cJSON* ia   = cJSON_CreateObject();

  cJSON_AddStringToObject(msg, "role", "user");
  cJSON_AddStringToObject(part, "type", "input_audio");

  body_cap = b64_len + 512u;
  body = (char*)malloc(body_cap);
  if (body == NULL)
    {
      cJSON_Delete(root);
      root = NULL;
      ret = -ENOMEM;
      goto out;
    }

  snprintf(body, body_cap, "data:audio/wav;base64,%s", (char*)b64);
  cJSON_AddStringToObject(ia, "data", body);
  cJSON_AddItemToObject(part, "input_audio", ia);
  cJSON_AddItemToArray(cont, part);
  cJSON_AddItemToObject(msg, "content", cont);
  cJSON_AddItemToArray(msgs, msg);
  cJSON_AddItemToObject(root, "messages", msgs);

  {
    cJSON* opts = cJSON_CreateObject();
    cJSON_AddStringToObject(opts, "language", "zh");
    cJSON_AddItemToObject(root, "asr_options", opts);
  }

  free(body);
  body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  root = NULL;

  if (body == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  resp = (char*)malloc(MIMO_ASR_RESP_CAP);
  if (resp == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  hdrs[0].name  = "Authorization";
  hdrs[0].value = s_auth;
  hdrs[1].name  = NULL;
  hdrs[1].value = NULL;

  syslog(LOG_INFO,
         "[%s] POST wav=%u bytes (%uHz, %.1fs speech), body=%u bytes\n",
         MIMO_ASR_TAG, (unsigned)wav_len, (unsigned)MIMO_ASR_RATE_OUT,
         (double)(wav_len - MIMO_ASR_WAV_HDR) /
             (double)(MIMO_ASR_RATE_OUT * 2u),
         (unsigned)strlen(body));

  status = vela_https_post_json(MIMO_API_HOST, MIMO_API_PORT, MIMO_API_PATH,
                                hdrs, body, resp, MIMO_ASR_RESP_CAP);

  if (status != 200)
    {
      syslog(LOG_ERR, "[%s] HTTP %d: %.160s\n", MIMO_ASR_TAG, status, resp);
      ret = (status < 0) ? status : -EIO;
      goto out;
    }

  root = cJSON_Parse(resp);
  if (root == NULL)
    {
      syslog(LOG_ERR, "[%s] bad JSON: %.160s\n", MIMO_ASR_TAG, resp);
      ret = -EPROTO;
      goto out;
    }

  cJSON* choices = cJSON_GetObjectItem(root, "choices");
  cJSON* first   = (choices != NULL && cJSON_IsArray(choices))
                   ? cJSON_GetArrayItem(choices, 0) : NULL;
  cJSON* message = (first != NULL) ? cJSON_GetObjectItem(first, "message") : NULL;
  cJSON* content = (message != NULL) ? cJSON_GetObjectItem(message, "content") : NULL;

  if (content == NULL || !cJSON_IsString(content))
    {
      syslog(LOG_ERR, "[%s] no content: %.160s\n", MIMO_ASR_TAG, resp);
      ret = -ENODATA;
      goto out;
    }

  strlcpy(text_out, content->valuestring, text_cap);

  syslog(LOG_INFO, "[%s] text=\"%s\"\n", MIMO_ASR_TAG, text_out);
  ret = 0;

out:
  if (root != NULL)
    {
      cJSON_Delete(root);
    }

  free(b64);
  free(wav);
  free(resp);
  free(body);
  return ret;
}

/* ── 批量接口 ── */
static int mimo_asr_recognize(const unsigned char* pcm_data, size_t pcm_len,
                              char* text_out, size_t text_cap)
{
  return mimo_asr_transcribe(pcm_data, pcm_len, text_out, text_cap);
}

/* ── 流式接口: VAD 边收边裁, 只攒"有人声"的 PCM, finish 时一次上传 ── */
struct mimo_asr_stream_s
{
  vad_trim_t     vad;
  unsigned char* pcm;
  size_t         len;
  size_t         cap;
  int            overflow;
};

static void mimo_asr_emit(void* user, const short* frame, size_t samples)
{
  struct mimo_asr_stream_s* st = (struct mimo_asr_stream_s*)user;
  const unsigned char* bytes = (const unsigned char*)frame;
  size_t len = samples * sizeof(short);

  if (st->overflow || bytes == NULL || len == 0)
    {
      return;
    }

  if (st->len + len > MIMO_ASR_MAX_PCM)
    {
      st->overflow = 1;
      return;
    }

  if (st->len + len > st->cap)
    {
      size_t ncap = st->cap ? st->cap * 2u : (32u * 1024u);
      unsigned char* np;

      if (ncap < st->len + len)
        {
          ncap = st->len + len;
        }

      np = (unsigned char*)realloc(st->pcm, ncap);
      if (np == NULL)
        {
          st->overflow = 1;
          return;
        }

      st->pcm = np;
      st->cap = ncap;
    }

  memcpy(st->pcm + st->len, bytes, len);
  st->len += len;
}

static void* mimo_asr_stream_open(void)
{
  struct mimo_asr_stream_s* st;

  if (!s_ready && mimo_asr_init() != 0)
    {
      return NULL;
    }

  st = (struct mimo_asr_stream_s*)calloc(1, sizeof(*st));
  if (st == NULL)
    {
      return NULL;
    }

  vad_trim_init(&st->vad);
  return st;
}

static int mimo_asr_stream_send(void* s, const unsigned char* pcm, size_t len)
{
  struct mimo_asr_stream_s* st = (struct mimo_asr_stream_s*)s;

  if (st == NULL || pcm == NULL)
    {
      return -EINVAL;
    }

  vad_trim_feed(&st->vad, (const short*)(const void*)pcm, len / 2,
                mimo_asr_emit, st);
  return 0;
}

static int mimo_asr_stream_finish(void* s, char* text_out, size_t text_cap)
{
  struct mimo_asr_stream_s* st = (struct mimo_asr_stream_s*)s;
  int ret;

  if (st == NULL)
    {
      return -EINVAL;
    }

  if (vad_trim_finish(&st->vad) != 0)
    {
      syslog(LOG_WARNING, "[%s] no speech detected, skip upload\n",
             MIMO_ASR_TAG);
      free(st->pcm);
      free(st);

      if (text_out != NULL && text_cap > 0)
        {
          text_out[0] = '\0';
        }

      return -ENODATA;
    }

  syslog(LOG_INFO, "[%s] VAD: kept %lu frames (%.1fs speech), pcm=%u bytes%s\n",
         MIMO_ASR_TAG, st->vad.emitted,
         (double)st->vad.emitted * 0.02,
         (unsigned)st->len, st->overflow ? " (truncated)" : "");

  ret = mimo_asr_transcribe(st->pcm, st->len, text_out, text_cap);

  free(st->pcm);
  free(st);
  return ret;
}

static void mimo_asr_stream_abort(void* s)
{
  struct mimo_asr_stream_s* st = (struct mimo_asr_stream_s*)s;

  if (st != NULL)
    {
      free(st->pcm);
      free(st);
    }
}

/* 自动断句: VAD 判定"一句话说完了"就返回 1。 */
static int mimo_asr_stream_utterance_done(void* s)
{
  struct mimo_asr_stream_s* st = (struct mimo_asr_stream_s*)s;

  return st ? vad_trim_utterance_done(&st->vad) : 0;
}

static const voice_asr_ops_t s_mimo_asr_ops =
{
  .name                  = "mimo",
  .init                  = mimo_asr_init,
  .recognize             = mimo_asr_recognize,
  .deinit                = NULL,
  .stream_open           = mimo_asr_stream_open,
  .stream_send           = mimo_asr_stream_send,
  .stream_finish         = mimo_asr_stream_finish,
  .stream_abort          = mimo_asr_stream_abort,
  .stream_end_input      = NULL,
  .stream_utterance_done = mimo_asr_stream_utterance_done,
};

int mimo_asr_register(void)
{
  return voice_asr_register(&s_mimo_asr_ops);
}
