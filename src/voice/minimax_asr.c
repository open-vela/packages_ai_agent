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
#include "voice/opus_ogg.h"
#include "voice/vad_trim.h"

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
/* 8s：原 40s 在弱网下会卡 46 秒才失败（真机 2026-10-25），
 * 用户完全不知道发生了什么。宁可快速失败并提示重说。 */
#define MM_ASR_TIMEOUT_MS 8000
#define MM_ASR_RESP_MAX   4096
#define MM_ASR_KEY_MAX    256

/* 官方上限是 500s / 50MB。这里按 60s@16kHz/mono/16bit(=1.92MB) 设保护上限;
 * 正常一次测量只有 3~30s, 这道闸只是防止把整块堆吃光。 */
#define MM_ASR_MAX_PCM    (60u * 16000u * 2u)
#define MM_ASR_WAV_HDR    44u

/* 16kbps: 4 秒语音约 8KB, 而蓝牙 PAN 上行实测 4.6~4.9KB/s => 约 1.7 秒传完。
 * 官方文档明确"mono 16kHz 的 opus 精度不受影响", 主机上也实测过: 16kbps 的
 * Ogg Opus 识别结果与原 WAV **一字不差**。 */
#define MM_ASR_OPUS_BPS   16000

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

static int mm_asr_post_payload(const unsigned char* payload,
                               size_t payload_len, const char* mime,
                               const char* fname, char* text_out,
                               size_t text_cap);

static int mm_asr_recognize(const unsigned char* pcm_data, size_t pcm_len,
                            char* text_out, size_t text_cap)
{
  unsigned char* payload = NULL;       /* 要上传的"文件": Ogg Opus 或 WAV */
  size_t         payload_len = 0;
  const char*    mime = "audio/wav";
  const char*    fname = "a.wav";
  int            ret;

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

  /* ── 第一步: 准备要上传的"文件" ──────────────────────────────────────
   * 优先 Ogg Opus: 蓝牙 PAN 上行的结构上限约 12KB/s, 而 16kHz/16bit 原始 PCM
   * 是 32KB/s —— 4 秒语音要传 26 秒; Opus 16kbps 只要约 8KB, 约 1.7 秒。
   * **必须带 Ogg 容器**: 实测 MiniMax 拒绝无容器的裸 Opus 码流, 稳定返回
   * HTTP 400 "Invalid data found when processing input" (换 .ogg 后缀也一样,
   * 它看内容不看扩展名)。
   * 编码失败就回退到原来的 WAV 路径, 保证功能不中断。 */
  {
    unsigned char* ogg = NULL;
    size_t         ogg_len = 0;

    ret = opus_ogg_encode(pcm_data, pcm_len, MM_ASR_OPUS_BPS,
                          &ogg, &ogg_len);
    if (ret == 0 && ogg != NULL && ogg_len > 0)
      {
        payload = ogg;
        payload_len = ogg_len;
        mime = "audio/ogg";
        fname = "a.ogg";
        syslog(LOG_INFO,
               "[%s] pcm=%u bytes (%.1fs) -> Ogg Opus %u bytes\n",
               MM_ASR_TAG, (unsigned)pcm_len, (double)pcm_len / 32000.0,
               (unsigned)ogg_len);
      }
    else
      {
        syslog(LOG_ERR, "[%s] opus encode failed (%d), fallback to wav\n",
               MM_ASR_TAG, ret);
        ret = 0;
      }
  }

  if (payload == NULL)
    {
      unsigned char wav[MM_ASR_WAV_HDR];

      mm_asr_wav_header(wav, (unsigned long)pcm_len);
      payload = (unsigned char*)malloc(MM_ASR_WAV_HDR + pcm_len);
      if (payload == NULL)
        {
          return -ENOMEM;
        }

      memcpy(payload, wav, MM_ASR_WAV_HDR);
      memcpy(payload + MM_ASR_WAV_HDR, pcm_data, pcm_len);
      payload_len = MM_ASR_WAV_HDR + pcm_len;
      mime = "audio/wav";
      fname = "a.wav";
    }

  ret = mm_asr_post_payload(payload, payload_len, mime, fname,
                            text_out, text_cap);
  free(payload);
  return ret;
}

/* 把已经准备好的"文件"(Ogg Opus 或 WAV)组 multipart 传上去并解析回文字。
 * 批量识别与流式识别两条路径共用。 */
static int mm_asr_post_payload(const unsigned char* payload, size_t payload_len,
                               const char* mime, const char* fname,
                               char* text_out, size_t text_cap)
{
  unsigned char* body;
  char*          resp;
  size_t         body_cap;
  size_t         off;
  int            n;
  int            ret;
  cJSON*         root = NULL;
  cJSON*         item;

  if (payload == NULL || payload_len == 0 || mime == NULL || fname == NULL ||
      text_out == NULL || text_cap == 0)
    {
      return -EINVAL;
    }

  text_out[0] = '\0';

  /* 懒初始化: 注册时不联网, 第一次真正要识别才取 key */
  if (!s_ready && mm_asr_init() != 0)
    {
      return -ENOENT;
    }

  body_cap = payload_len + 1024u;
  body = (unsigned char*)malloc(body_cap);
  resp = (char*)malloc(MM_ASR_RESP_MAX);

  if (body == NULL || resp == NULL)
    {
      free(body);
      free(resp);
      return -ENOMEM;
    }

  n = snprintf((char*)body, body_cap,
               "--" MM_ASR_BOUNDARY "\r\n"
               "Content-Disposition: form-data; name=\"model\"\r\n"
               "\r\n"
               MM_ASR_MODEL "\r\n"
               "--" MM_ASR_BOUNDARY "\r\n"
               "Content-Disposition: form-data; name=\"file\"; "
               "filename=\"%s\"\r\n"
               "Content-Type: %s\r\n"
               "\r\n",
               fname, mime);

  if (n <= 0 || (size_t)n + payload_len + 8u > body_cap)
    {
      ret = -EOVERFLOW;
      goto out;
    }

  off = (size_t)n;
  memcpy(body + off, payload, payload_len);
  off += payload_len;

  n = snprintf((char*)body + off, body_cap - off,
               "\r\n--" MM_ASR_BOUNDARY "--\r\n");
  if (n <= 0)
    {
      ret = -EOVERFLOW;
      goto out;
    }

  off += (size_t)n;

  syslog(LOG_INFO, "[%s] POST %s: %s %u bytes, body=%u bytes\n",
         MM_ASR_TAG, MM_ASR_PATH, mime, (unsigned)payload_len,
         (unsigned)off);

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

/* ── 流式: 边采集边编码 ────────────────────────────────────────────────
 * 采集线程每收到一块 PCM 就 stream_send() 进来, 这里立刻编成 Opus 入 Ogg。
 * complexity=0 实测 2.05x 实时, 有 2 倍余量跟得上采集, 录完时只剩上传,
 * 省掉批量路径那整段约 2 秒的编码等待。
 * MiniMax 的 ASR 是**文件式**的(不支持音频侧流式输入, 官方也建议客户端做
 * VAD 伪流式), 所以"流"只到设备侧的编码为止, 上传仍在 finish 里一次完成。 */

/* 流式会话 = Opus 编码器 + VAD。
 * VAD 只把"有语音"的帧放行给编码器, 首尾静音直接丢掉 —— 真机上说话人只说了
 * 3 秒却录了 22 秒, 送给识别引擎的音频 85% 是噪声, 上传量也从约 6KB 涨到 42KB。
 * 只裁首尾, 中间一律放行(中间一裁会切断词间停顿, 反而伤识别), 详见 vad_trim.c。 */
struct mm_asr_stream_s
{
  opus_ogg_enc_t* enc;
  vad_trim_t      vad;
  int             err;          /* 回调里没法返回错误, 先记在这儿 */
};

static void mm_asr_emit(void* user, const short* frame, size_t samples)
{
  struct mm_asr_stream_s* st = (struct mm_asr_stream_s*)user;

  if (st->err == 0)
    {
      st->err = opus_ogg_feed(st->enc, frame, samples * sizeof(short));
    }
}

static void* mm_asr_stream_open(void)
{
  struct mm_asr_stream_s* st;

  if (!s_ready && mm_asr_init() != 0)
    {
      return NULL;
    }

  st = (struct mm_asr_stream_s*)calloc(1, sizeof(*st));
  if (st == NULL)
    {
      return NULL;
    }

  st->enc = opus_ogg_begin(MM_ASR_OPUS_BPS);
  if (st->enc == NULL)
    {
      free(st);
      return NULL;
    }

  vad_trim_init(&st->vad);
  return st;
}

static int mm_asr_stream_send(void* s, const unsigned char* pcm, size_t len)
{
  struct mm_asr_stream_s* st = (struct mm_asr_stream_s*)s;

  if (st == NULL || st->enc == NULL || pcm == NULL)
    {
      return -EINVAL;
    }

  st->err = 0;
  vad_trim_feed(&st->vad, (const short*)(const void*)pcm, len / 2,
                mm_asr_emit, st);
  return st->err;
}

static int mm_asr_stream_finish(void* s, char* text_out, size_t text_cap)
{
  struct mm_asr_stream_s* st = (struct mm_asr_stream_s*)s;
  unsigned char* ogg = NULL;
  size_t         ogg_len = 0;
  int            ret;

  if (st == NULL)
    {
      return -EINVAL;
    }

  if (vad_trim_finish(&st->vad) != 0)
    {
      /* 全段没有语音: 不上传。传一个只有头页的空 Ogg 上去, 服务端会直接拒掉
       * (真机 2026-10-14 实测: request failed -5), 白费一次往返。 */
      syslog(LOG_WARNING, "[%s] no speech detected, skip upload\n",
             MM_ASR_TAG);
      opus_ogg_abort(st->enc);
      free(st);

      if (text_out != NULL && text_cap > 0)
        {
          text_out[0] = 0;
        }

      return -ENODATA;
    }

  syslog(LOG_INFO, "[%s] VAD: kept %lu frames, dropped %lu (%.1fs speech)\n",
         MM_ASR_TAG, st->vad.emitted, st->vad.dropped,
         (double)st->vad.emitted * 0.02);

  ret = opus_ogg_finish(st->enc, &ogg, &ogg_len);
  free(st);

  if (ret != 0 || ogg == NULL || ogg_len == 0)
    {
      syslog(LOG_ERR, "[%s] stream finish failed: %d\n", MM_ASR_TAG, ret);
      free(ogg);
      return ret != 0 ? ret : -EIO;
    }

  ret = mm_asr_post_payload(ogg, ogg_len, "audio/ogg", "a.ogg",
                            text_out, text_cap);
  free(ogg);
  return ret;
}

/* 由采集线程(64KB 栈)调用: 把最后不足 20ms 的残余补零编掉。
 * 不放在 finish 里做是因为 finish 跑在调用者的线程上(真机是 16KB 的 CLI
 * 线程), 在那里调 opus_encode 会栈溢出硬 fault。 */
static int mm_asr_stream_end_input(void* s)
{
  struct mm_asr_stream_s* st = (struct mm_asr_stream_s*)s;

  return st ? opus_ogg_end_input(st->enc) : -EINVAL;
}

/* 自动断句: VAD 判定"一句话说完了"就返回 1, 采集线程据此自动收尾。 */
static int mm_asr_stream_utterance_done(void* s)
{
  struct mm_asr_stream_s* st = (struct mm_asr_stream_s*)s;

  return st ? vad_trim_utterance_done(&st->vad) : 0;
}

static void mm_asr_stream_abort(void* s)
{
  struct mm_asr_stream_s* st = (struct mm_asr_stream_s*)s;

  if (st == NULL)
    {
      return;
    }

  opus_ogg_abort(st->enc);
  free(st);
}

static const voice_asr_ops_t s_minimax_asr_ops =
{
  .name = "minimax",
  /* 流式: 让采集线程边录边编码 (见本文件上面的说明) */
  .stream_open = mm_asr_stream_open,
  .stream_send = mm_asr_stream_send,
  .stream_finish = mm_asr_stream_finish,
  .stream_abort = mm_asr_stream_abort,
  .stream_end_input = mm_asr_stream_end_input,
  .stream_utterance_done = mm_asr_stream_utterance_done,
  .init = mm_asr_init,
  .recognize = mm_asr_recognize,
  .deinit = NULL,
};

int minimax_asr_register(void)
{
  return voice_asr_register(&s_minimax_asr_ops);
}
