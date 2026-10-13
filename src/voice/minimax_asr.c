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
 * MiniMax 语音识别 (Speech-to-Text) 后端
 *
 * 为什么是"文件式 REST"而不是流式 WebSocket:
 *   本板已有的一条真机验证过的通道就是 minimax_tts.c 里的 HTTPS POST
 *   (同一个 host、同一个 key、同一套代理隧道)。ASR 走同一条路, 不需要再
 *   引一套 WebSocket 状态机; 代价是一次性上传整段音频 —— 对"按住说一句"
 *   这种交互完全够用。
 *
 * 为什么要套 WAV 头:
 *   官方明确列出"不支持无容器的裸 PCM"(支持 wav/aiff/flac/m4a/mp3/aac/
 *   opus/ogg)。设备上录到的正好是 16kHz/mono/16bit 裸 PCM, 也正是官方建议
 *   的规格, 所以只加 44 字节标准 RIFF 头, 不做任何编码。
 ****************************************************************************/

#include "voice/voice_asr.h"
#include "voice/minimax_tts.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "cJSON.h"

#define MM_ASR_TAG        "minimax_asr"
#define MM_ASR_PATH       "/v1/speech_to_text"
#define MM_ASR_MODEL      "asr-1.0"
#define MM_ASR_BOUNDARY   "----VelaMiniMaxAsrBoundary4b7c"
#define MM_ASR_TIMEOUT_MS 40000
#define MM_ASR_RESP_MAX   4096
#define MM_ASR_KEY_MAX    256

/* 官方上限是 500s / 50MB。这里按 60s@16kHz/mono/16bit(=1.92MB) 设保护上限;
 * 正常一次测量只有 3~30s, 这道闸只是防止把整块堆吃光。 */
#define MM_ASR_MAX_PCM    (60u * 16000u * 2u)
#define MM_ASR_WAV_HDR    44u

static char s_key[MM_ASR_KEY_MAX];
static int  s_ready;

static void mm_asr_le16(unsigned char* p, unsigned v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
}

static void mm_asr_le32(unsigned char* p, unsigned long v)
{
  p[0] = (unsigned char)(v & 0xff);
  p[1] = (unsigned char)((v >> 8) & 0xff);
  p[2] = (unsigned char)((v >> 16) & 0xff);
  p[3] = (unsigned char)((v >> 24) & 0xff);
}

/* 44 字节标准 WAV 头: RIFF/WAVE + fmt(16, PCM, mono, 16000Hz, 16bit) + data */
static void mm_asr_wav_header(unsigned char* h, unsigned long pcm_len)
{
  memcpy(h, "RIFF", 4);
  mm_asr_le32(h + 4, 36u + pcm_len);
  memcpy(h + 8, "WAVEfmt ", 8);
  mm_asr_le32(h + 16, 16u);              /* fmt chunk size */
  mm_asr_le16(h + 20, 1u);               /* PCM */
  mm_asr_le16(h + 22, 1u);               /* mono */
  mm_asr_le32(h + 24, 16000u);           /* sample rate */
  mm_asr_le32(h + 28, 16000u * 2u);      /* byte rate */
  mm_asr_le16(h + 32, 2u);               /* block align */
  mm_asr_le16(h + 34, 16u);              /* bits per sample */
  memcpy(h + 36, "data", 4);
  mm_asr_le32(h + 40, pcm_len);
}

static int mm_asr_init(void)
{
  if (minimax_get_api_key(s_key, sizeof(s_key)) != 0 || s_key[0] == '\0')
    {
      syslog(LOG_ERR, "[%s] no MiniMax API key "
             "(tts_api_key / api_key / llm_backend_0 / local)\n", MM_ASR_TAG);
      s_ready = 0;
      return -ENOENT;
    }

  s_ready = 1;
  syslog(LOG_INFO, "[%s] ready (model " MM_ASR_MODEL ", key len=%u)\n",
         MM_ASR_TAG, (unsigned)strlen(s_key));
  return 0;
}

static int mm_asr_recognize(const unsigned char* pcm_data, size_t pcm_len,
                            char* text_out, size_t text_cap)
{
  unsigned char  wav[MM_ASR_WAV_HDR];
  unsigned char* body = NULL;
  char*          resp = NULL;
  size_t         body_cap;
  size_t         off;
  int            n;
  int            ret;
  cJSON*         root = NULL;
  cJSON*         item;

  if (text_out == NULL || text_cap == 0)
    {
      return -EINVAL;
    }

  text_out[0] = '\0';

  /* 懒初始化: 注册时不联网, 第一次真正要识别才取 key */
  if (!s_ready && mm_asr_init() != 0)
    {
      return -ENOENT;
    }

  if (pcm_data == NULL || pcm_len == 0)
    {
      return -EINVAL;
    }

  if (pcm_len > MM_ASR_MAX_PCM)
    {
      syslog(LOG_ERR, "[%s] pcm too long: %u\n", MM_ASR_TAG,
             (unsigned)pcm_len);
      return -E2BIG;
    }

  mm_asr_wav_header(wav, (unsigned long)pcm_len);

  /* multipart 体 = 前言 + model 段 + file 段(44B WAV 头 + PCM) + 结束行。
   * 512 的余量足够装下所有 boundary/CRLF 与段头。 */
  body_cap = pcm_len + MM_ASR_WAV_HDR + 512u;
  body = (unsigned char*)malloc(body_cap);
  resp = (char*)malloc(MM_ASR_RESP_MAX);

  if (body == NULL || resp == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  n = snprintf((char*)body, body_cap,
               "--" MM_ASR_BOUNDARY "\r\n"
               "Content-Disposition: form-data; name=\"model\"\r\n"
               "\r\n"
               MM_ASR_MODEL "\r\n"
               "--" MM_ASR_BOUNDARY "\r\n"
               "Content-Disposition: form-data; name=\"file\"; "
               "filename=\"a.wav\"\r\n"
               "Content-Type: audio/wav\r\n"
               "\r\n");

  if (n <= 0 || (size_t)n + MM_ASR_WAV_HDR + pcm_len + 8u > body_cap)
    {
      ret = -EOVERFLOW;
      goto out;
    }

  off = (size_t)n;
  memcpy(body + off, wav, MM_ASR_WAV_HDR);
  off += MM_ASR_WAV_HDR;
  memcpy(body + off, pcm_data, pcm_len);
  off += pcm_len;

  n = snprintf((char*)body + off, body_cap - off,
               "\r\n--" MM_ASR_BOUNDARY "--\r\n");
  if (n <= 0)
    {
      ret = -EOVERFLOW;
      goto out;
    }

  off += (size_t)n;

  syslog(LOG_INFO, "[%s] POST %s: pcm=%u bytes (%.1fs), body=%u bytes\n",
         MM_ASR_TAG, MM_ASR_PATH, (unsigned)pcm_len,
         (double)pcm_len / 32000.0, (unsigned)off);

  ret = minimax_https_post(MM_ASR_PATH,
                           "multipart/form-data; boundary=" MM_ASR_BOUNDARY,
                           s_key, body, off, resp, MM_ASR_RESP_MAX,
                           MM_ASR_TIMEOUT_MS);
  if (ret != 0)
    {
      syslog(LOG_ERR, "[%s] request failed: %d\n", MM_ASR_TAG, ret);
      goto out;
    }

  root = cJSON_Parse(resp);
  if (root == NULL)
    {
      syslog(LOG_ERR, "[%s] bad JSON: %.160s\n", MM_ASR_TAG, resp);
      ret = -EPROTO;
      goto out;
    }

  /* 业务错误在 base_resp.status_code, 0 才是成功 */
  item = cJSON_GetObjectItem(root, "base_resp");
  if (item != NULL)
    {
      cJSON* sc = cJSON_GetObjectItem(item, "status_code");

      if (sc != NULL && cJSON_IsNumber(sc) && sc->valueint != 0)
        {
          cJSON* msg = cJSON_GetObjectItem(item, "status_msg");

          syslog(LOG_ERR, "[%s] api error %d: %s\n", MM_ASR_TAG,
                 sc->valueint,
                 (msg != NULL && cJSON_IsString(msg)) ? msg->valuestring : "?");
          snprintf(text_out, text_cap, "(ASR api error %d)", sc->valueint);
          ret = -EIO;
          goto out;
        }
    }

  item = cJSON_GetObjectItem(root, "text");
  if (item == NULL || !cJSON_IsString(item))
    {
      syslog(LOG_ERR, "[%s] no text field: %.160s\n", MM_ASR_TAG, resp);
      ret = -ENODATA;
      goto out;
    }

  strlcpy(text_out, item->valuestring, text_cap);

  syslog(LOG_INFO, "[%s] text=\"%s\"\n", MM_ASR_TAG, text_out);
  ret = 0;

out:
  if (root != NULL)
    {
      cJSON_Delete(root);
    }

  free(body);
  free(resp);
  return ret;
}

static const voice_asr_ops_t s_minimax_asr_ops =
{
  .name = "minimax",
  .init = mm_asr_init,
  .recognize = mm_asr_recognize,
  .deinit = NULL,
};

int minimax_asr_register(void)
{
  return voice_asr_register(&s_minimax_asr_ops);
}
