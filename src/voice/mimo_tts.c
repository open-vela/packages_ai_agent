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
 * Xiaomi MiMo TTS backend (mimo-v2.5-tts, Token Plan gateway)
 *
 * 用 MiMo 的 OpenAI 兼容 chat/completions 做语音合成:
 *   POST https://token-plan-cn.xiaomimimo.com/v1/chat/completions
 *   {
 *     "model": "mimo-v2.5-tts",
 *     "messages": [ { "role": "assistant", "content": "<要念的文本>" } ],
 *     "audio": { "format": "mp3", "voice": "mimo_default" }
 *   }
 * 响应里 choices[0].message.audio.data 是 **base64 的 MP3**(24kHz mono),
 * 用现有的 edge_mp3 解码器解成 PCM 后逐块喂给播放回调。
 *
 * ── DEMO-VOICE-B（2026-11-03）: 新增 PCM16 流式合成路径 ──
 * MiMo 官方支持 stream:true 且流式时音频以 SSE delta.audio.data(base64)
 * 增量返回; 官方建议流式时用 audio.format=pcm16 (24kHz PCM16LE mono)。
 * 新路径在收到第一段音频后就开始逐块写播放器, 首声不再等整段 MP3 下载+解码。
 * 官方文档/本仓 87/89 号调研同时警告: PCM16@24k 净码率 48KB/s (base64 后
 * ~64KB/s) 远高于 BT PAN 实测带宽, 长句在链路上必然欠载 —— 因此:
 *   - 首个回调前预灌一小段 (MIMO_TTS_PREBUFFER_MS) 抗抖动;
 *   - 播放器一旦开始, 后续块仍然按到达节奏写;
 *   - 流式失败/非 200/没有 audio delta → 整条回退下方原有的非流式 MP3,
 *     行为与改动前完全一致;
 *   - 编译开关 MIMO_TTS_PCM16_STREAM 可一键回滚到非流式。
 ****************************************************************************/

#include "voice/voice_tts.h"
#include "voice/edge_mp3.h"
#include "voice/mimo_tts.h"
#include "voice/mimo_key_local.h"
#include "infra/vela_tls.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>

#include <mbedtls/base64.h>

#include "cJSON.h"

#define MIMO_TTS_TAG       "mimo_tts"

/* v142 Fix2：非流式 MiMo TTS 的 HTTP 尝试次数。
 * 截断（VELA_TLS_ERR_TRUNCATED）、传输错误或"HTTP 200 但 JSON 坏掉"时，
 * 只要一个 PCM 字节都还没播出，就丢连接池连接再试一次；已播出音频不重放。
 * 回滚开关：设为 1 即恢复"单次尝试"旧行为。 */
#ifndef MIMO_TTS_HTTP_ATTEMPTS
#define MIMO_TTS_HTTP_ATTEMPTS 2
#endif

/* 一句 100 字播报约 100~200KB MP3, base64 后 ~140~270KB; 给 512KB 余量。 */
#define MIMO_TTS_RESP_CAP  (512u * 1024u)
#define MIMO_TTS_BODY_CAP  (8u * 1024u)

static char s_auth[128];   /* "Bearer tp-..." */
static int  s_ready;

static int mimo_tts_init(void)
{
  if (MIMO_API_KEY_LOCAL[0] == '\0')
    {
      syslog(LOG_ERR, "[%s] no build-time MiMo key\n", MIMO_TTS_TAG);
      return -ENOENT;
    }

  if (s_auth[0] == '\0')
    {
      snprintf(s_auth, sizeof(s_auth), "Bearer %s", MIMO_API_KEY_LOCAL);
    }

  s_ready = 1;
  syslog(LOG_INFO, "[%s] ready (model %s, voice %s, host %s)\n",
         MIMO_TTS_TAG, MIMO_TTS_MODEL, MIMO_TTS_VOICE, MIMO_API_HOST);
  return 0;
}

/* edge_mp3 解码回调 -> 播放回调 */
struct mimo_tts_sink_s
{
  voice_tts_chunk_cb cb;
  void*              user;
  size_t             pcm_bytes;
  int                chunks;
};

static void mimo_tts_pcm_cb(const unsigned char* pcm, size_t len, void* user_data)
{
  struct mimo_tts_sink_s* sink = (struct mimo_tts_sink_s*)user_data;

  if (pcm == NULL || len == 0)
    {
      return;
    }

  sink->pcm_bytes += len;
  sink->chunks++;

  if (sink->cb != NULL)
    {
      sink->cb(pcm, len, 0, sink->user);
    }
}

/* 原有非流式实现（整段 MP3 下载 + edge_mp3 解码）。
 * DEMO-VOICE-B 之后它只作为 PCM16 流式路径的回退。 */
static int mimo_tts_synthesize_mp3(const char* text,
                                   voice_tts_chunk_cb cb, void* user_data)
{
  cJSON* root = NULL;
  char*  body = NULL;
  char*  resp = NULL;
  unsigned char* mp3 = NULL;
  size_t mp3_len = 0;
  edge_mp3_t* dec = NULL;
  struct mimo_tts_sink_s sink;
  vela_header_t hdrs[2];
  int   status = -EIO;
  int   attempt;
  int   ret = -EIO;

  if (!s_ready && mimo_tts_init() != 0)
    {
      return -ENOENT;
    }

  if (text == NULL || text[0] == '\0')
    {
      return -EINVAL;
    }

  memset(&sink, 0, sizeof(sink));
  sink.cb = cb;
  sink.user = user_data;

  /* ── 组请求 ── */
  root = cJSON_CreateObject();
  if (root == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(root, "model", MIMO_TTS_MODEL);

  cJSON* msgs = cJSON_CreateArray();
  cJSON* msg  = cJSON_CreateObject();
  cJSON_AddStringToObject(msg, "role", "assistant");
  cJSON_AddStringToObject(msg, "content", text);
  cJSON_AddItemToArray(msgs, msg);
  cJSON_AddItemToObject(root, "messages", msgs);

  cJSON* audio = cJSON_CreateObject();
  cJSON_AddStringToObject(audio, "format", "mp3");
  cJSON_AddStringToObject(audio, "voice", MIMO_TTS_VOICE);
  cJSON_AddItemToObject(root, "audio", audio);

  body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  root = NULL;

  if (body == NULL)
    {
      return -ENOMEM;
    }

  hdrs[0].name  = "Authorization";
  hdrs[0].value = s_auth;
  hdrs[1].name  = NULL;
  hdrs[1].value = NULL;

  syslog(LOG_INFO, "[%s] POST %s (%u body bytes, text %u chars)\n",
         MIMO_TTS_TAG, MIMO_API_PATH, (unsigned)strlen(body),
         (unsigned)strlen(text));

  /* v142 Fix2：TLS 截断（VELA_TLS_ERR_TRUNCATED=-6）/ 传输错误 / "HTTP 200
   * 但 JSON 坏掉" 且**一个 PCM 字节都还没播出**时，丢弃连接池里可能带残留
   * body 的连接（vela_tls_pool_invalidate）后重试一次。
   *
   * 背景（.v3_work/plan/reset_rca.md）：v132 真机 cl=46119/body=34797 被旧
   * vela_tls 当 200 返回 -> 这里 bad JSON(-EPROTO) -> 直接进 voice_tts 回退链
   * （minimax 必死/Edge -0x4C 30s/volc 无凭证）-> ~70s 后整机看门狗复位。
   * 重试一次 TTS 请求（≤46KB）远小于回退链代价。
   *
   * 注意：只在"尚未播出 PCM"时重试；已播出部分沿用后面的"可播即播"策略，
   * 不重放。回滚开关：MIMO_TTS_HTTP_ATTEMPTS=1。 */
  for (attempt = 0; attempt < MIMO_TTS_HTTP_ATTEMPTS; attempt++)
    {
      resp = (char*)malloc(MIMO_TTS_RESP_CAP);
      if (resp == NULL)
        {
          free(body);
          body = NULL;
          return -ENOMEM;
        }

      status = vela_https_post_json(MIMO_API_HOST, MIMO_API_PORT,
                                    MIMO_API_PATH, hdrs, body, resp,
                                    MIMO_TTS_RESP_CAP);

      if (status == 200)
        {
          root = cJSON_Parse(resp);
          if (root != NULL)
            {
              break;                     /* 成功，跳出重试循环 */
            }
          syslog(LOG_ERR, "[%s] bad JSON (attempt %d/%d): %.160s\n",
                 MIMO_TTS_TAG, attempt + 1, MIMO_TTS_HTTP_ATTEMPTS, resp);
        }
      else
        {
          syslog(LOG_ERR, "[%s] HTTP %d (attempt %d/%d): %.160s\n",
                 MIMO_TTS_TAG, status, attempt + 1, MIMO_TTS_HTTP_ATTEMPTS,
                 resp);
        }

      free(resp);
      resp = NULL;
      if (root != NULL)
        {
          cJSON_Delete(root);
          root = NULL;
        }

      if (attempt + 1 >= MIMO_TTS_HTTP_ATTEMPTS)
        {
          break;
        }

      syslog(LOG_WARNING,
             "[%s] retry after truncated/bad response "
             "(status=%d, attempt %d/%d)\n",
             MIMO_TTS_TAG, status, attempt + 1, MIMO_TTS_HTTP_ATTEMPTS);
      vela_tls_pool_invalidate();   /* 丢弃可能带残留 body 的池连接 */
    }

  free(body);
  body = NULL;

  if (root == NULL)
    {
      ret = (status < 0) ? status : -EPROTO;
      goto out;
    }

  cJSON* choices = cJSON_GetObjectItem(root, "choices");
  cJSON* first   = (choices != NULL && cJSON_IsArray(choices))
                   ? cJSON_GetArrayItem(choices, 0) : NULL;
  cJSON* message = (first != NULL) ? cJSON_GetObjectItem(first, "message") : NULL;
  cJSON* au      = (message != NULL) ? cJSON_GetObjectItem(message, "audio") : NULL;
  cJSON* data    = (au != NULL) ? cJSON_GetObjectItem(au, "data") : NULL;

  if (data == NULL || !cJSON_IsString(data) || data->valuestring[0] == '\0')
    {
      syslog(LOG_ERR, "[%s] no audio data: %.160s\n", MIMO_TTS_TAG, resp);
      ret = -ENODATA;
      goto out;
    }

  {
    size_t b64_len = strlen(data->valuestring);
    size_t cap = (b64_len / 4u + 1u) * 3u;

    mp3 = (unsigned char*)malloc(cap);
    if (mp3 == NULL)
      {
        ret = -ENOMEM;
        goto out;
      }

    if (mbedtls_base64_decode(mp3, cap, &mp3_len,
                              (const unsigned char*)data->valuestring,
                              b64_len) != 0)
      {
        syslog(LOG_ERR, "[%s] base64 decode failed (b64 %u)\n",
               MIMO_TTS_TAG, (unsigned)b64_len);
        ret = -EPROTO;
        goto out;
      }
  }

  /* 响应体用完即可释放, 后面只剩 MP3 缓冲 */
  cJSON_Delete(root);
  root = NULL;
  free(resp);
  resp = NULL;

  syslog(LOG_INFO, "[%s] MP3 %u bytes -> decode\n", MIMO_TTS_TAG,
         (unsigned)mp3_len);

  dec = edge_mp3_open(mimo_tts_pcm_cb, &sink);
  if (dec == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }

  if (edge_mp3_feed(dec, mp3, mp3_len) != 0)
    {
      syslog(LOG_ERR, "[%s] mp3 feed failed\n", MIMO_TTS_TAG);
      ret = -EIO;
      goto out;
    }

  syslog(LOG_INFO, "[%s] done: mp3=%u pcm=%u chunks=%d %dHz %dch\n",
         MIMO_TTS_TAG, (unsigned)edge_mp3_input_bytes(dec),
         (unsigned)sink.pcm_bytes, sink.chunks,
         edge_mp3_sample_rate(dec), edge_mp3_channels(dec));

  ret = 0;

out:
  if (dec != NULL)
    {
      edge_mp3_close(dec);
    }

  if (ret == 0 && cb != NULL)
    {
      cb(NULL, 0, 1, user_data);       /* 收尾: 通知播放层本句结束 */
    }

  if (root != NULL)
    {
      cJSON_Delete(root);
    }

  free(resp);
  free(mp3);
  free(body);
  return ret;
}

/* ═══════════════════════════════════════════════════════════════
 * DEMO-VOICE-B: PCM16 流式合成（SSE + base64 增量）
 *
 * 请求体在非流式基础上加 "stream": true，并把 audio.format 换成 pcm16。
 * 响应是 SSE: 每行 "data: {...}"，音频在
 *   choices[0].delta.audio.data   (base64)
 * 里增量返回；跨 SSE chunk 的 base64 余数（4 字符边界）必须缓存后拼接。
 * 解出的 PCM16LE 24kHz mono 逐块交给 voice_tts 回调（voice_channel 的
 * tts_stream_cb 直接 audio_playback_write）。
 *
 * 回退策略（任何一条命中都返回负值，让上层 call site 走原 MP3 路径）：
 *   - HTTP 非 200 / 连接失败 / 超时
 *   - 没有任何 audio delta（服务端可能忽略 stream:true 返回普通 JSON）
 *   - SSE/JSON/base64 解析失败（且此前一个音频字节都没播出）
 * 如果已经播出过 PCM 才失败，则"可播即播"：返回 0，不再重放一遍 MP3。
 * ═══════════════════════════════════════════════════════════════ */

#ifndef MIMO_TTS_PCM16_STREAM
/* 2026-11-03 真机实测：BT PAN 有效带宽约 12KB/s，而 PCM16 流需要 ~64KB/s；
 * 短句首声反而 3.9s（旧 MP3 2.5s），长句中途流失败只播 0.8s。演示版默认关闭，
 * 代码/开关保留，等链路改善（WiFi/4G）再开。 */
#define MIMO_TTS_PCM16_STREAM 0
#endif

#if MIMO_TTS_PCM16_STREAM

#define MIMO_TTS_SAMPLE_RATE       24000u
#define MIMO_TTS_SSE_LINE_INIT     (8u * 1024u)
#define MIMO_TTS_SSE_LINE_MAX      (96u * 1024u)
#define MIMO_TTS_PCM_FLUSH         2048u   /* 开始播放后每次回调的 PCM 字节数 */
/* 首个回调前预灌的音频时长。PCM16@24k mono = 48 B/ms：
 *   500ms = 24KB。链路比实时慢时这是唯一能吸收抖动的缓冲；调大首声变慢，
 *   调小更容易在句中出现欠载。真机判据见 [mimo_tts] first_pcm/first_audio。 */
#ifndef MIMO_TTS_PREBUFFER_MS
#define MIMO_TTS_PREBUFFER_MS      500
#endif
/* 整个流的总超时（不是单次读超时；vela_tls 另有 20s body idle 兜底）。 */
#ifndef MIMO_TTS_STREAM_TIMEOUT_MS
#define MIMO_TTS_STREAM_TIMEOUT_MS 30000
#endif
/* 单句 PCM 上限（安全阀：48KB/s，2MB≈42s，远超 TTS 90 字上限） */
#define MIMO_TTS_PCM_MAX_BYTES     (2u * 1024u * 1024u)

typedef struct
{
  /* SSE 行缓冲：一次 body 片段可能只有半行，需跨片段拼接 */
  char*  line;
  size_t line_len;
  size_t line_cap;

  /* base64 跨 chunk 余数（官方流式是按 4 字符组编码，但 SSE 切包不等长）。
   * 注意不要用 '=' 填充来"锁死"流：每个 SSE delta 既可能是同一个 base64
   * 连续流的一段，也可能是各自独立 padding 的一段 —— 两种都必须继续解。 */
  char   b64_carry[4];
  int    b64_carry_len;

  /* PCM 聚合缓冲：预灌够 / 攒到 FLUSH 字节再回调 */
  unsigned char* pcm;
  size_t pcm_len;
  size_t pcm_cap;
  size_t prebuffer_bytes;
  int    playback_started;

  size_t audio_b64_chars;
  size_t pcm_bytes;       /* 已交给播放回调的字节 */
  int    chunks;
  int    sse_lines;
  int    audio_deltas;
  int    parse_errors;
  int    saw_done;
  int    failed;
  int    fail_err;

  struct mimo_tts_sink_s* sink;
  struct timespec t0;
  long   first_pcm_ms;    /* 收到第一段可解码 audio delta 的时刻 */
  long   first_audio_ms;  /* 第一次真正交给播放器的时刻（含预灌） */
} mimo_pcm_stream_t;

static long mimo_tts_elapsed_ms(const struct timespec* t0)
{
  struct timespec now;

  clock_gettime(CLOCK_MONOTONIC, &now);
  return (now.tv_sec - t0->tv_sec) * 1000
       + (now.tv_nsec - t0->tv_nsec) / 1000000;
}

static int mimo_tts_b64_val(char c)
{
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

/* 解一组 4 字符 -> 1..3 字节；返回字节数，-1 非法。 */
static int mimo_tts_b64_group(const char g[4], unsigned char out[3])
{
  int v0, v1, v2, v3;
  int n = 3;

  v0 = mimo_tts_b64_val(g[0]);
  v1 = mimo_tts_b64_val(g[1]);
  if (v0 < 0 || v1 < 0)
    {
      return -1;
    }

  if (g[2] == '=')
    {
      if (g[3] != '=')
        {
          return -1;
        }
      n = 1;
      v2 = 0;
      v3 = 0;
    }
  else
    {
      v2 = mimo_tts_b64_val(g[2]);
      if (v2 < 0)
        {
          return -1;
        }
      if (g[3] == '=')
        {
          n = 2;
          v3 = 0;
        }
      else
        {
          v3 = mimo_tts_b64_val(g[3]);
          if (v3 < 0)
            {
              return -1;
            }
        }
    }

  out[0] = (unsigned char)((v0 << 2) | (v1 >> 4));
  out[1] = (unsigned char)(((v1 & 0x0F) << 4) | (v2 >> 2));
  out[2] = (unsigned char)(((v2 & 0x03) << 6) | v3);
  return n;
}

/* 把聚合缓冲按策略交给播放回调。
 * 未开始播放 -> 攒够 prebuffer 才发；已开始 -> 攒够 FLUSH 或 force 时发。 */
static void mimo_tts_pcm_deliver(mimo_pcm_stream_t* st, int force)
{
  size_t target;
  size_t n;

  /* 16-bit PCM：中间块保持偶数字节，奇数字节留到下次 */
  n = st->pcm_len & ~(size_t)1;
  if (n == 0 && !force)
    {
      return;
    }

  target = st->playback_started ? (size_t)MIMO_TTS_PCM_FLUSH
                                : st->prebuffer_bytes;
  if (!force && n < target)
    {
      return;
    }

  if (n == 0)
    {
      st->pcm_len = 0;
      return;
    }

  if (!st->playback_started)
    {
      st->playback_started = 1;
      st->first_audio_ms = mimo_tts_elapsed_ms(&st->t0);
      /* 验收打点：TTS 开始 -> 第一块 PCM 交给播放器的毫秒数/字节数 */
      syslog(LOG_INFO,
             "[%s] stream first audio chunk: %ldms (%u B, prebuffer %u B)\n",
             MIMO_TTS_TAG, st->first_audio_ms, (unsigned)n,
             (unsigned)st->prebuffer_bytes);
    }

  mimo_tts_pcm_cb(st->pcm, n, st->sink);
  st->pcm_bytes += n;
  st->chunks++;

  if (st->pcm_len > n)
    {
      st->pcm[0] = st->pcm[n];
    }
  st->pcm_len -= n;

  if (st->pcm_bytes > MIMO_TTS_PCM_MAX_BYTES)
    {
      if (!st->failed)
        {
          st->failed = 1;
          st->fail_err = -EOVERFLOW;
        }
    }
}

static int mimo_tts_pcm_append(mimo_pcm_stream_t* st,
                               const unsigned char* data, size_t len)
{
  if (st->pcm_len + len > st->pcm_cap)
    {
      mimo_tts_pcm_deliver(st, 1);
    }
  if (st->pcm_len + len > st->pcm_cap)
    {
      return -ENOMEM;
    }

  memcpy(st->pcm + st->pcm_len, data, len);
  st->pcm_len += len;

  if (!st->playback_started)
    {
      if (st->pcm_len >= st->prebuffer_bytes)
        {
          mimo_tts_pcm_deliver(st, 1);
        }
    }
  else if (st->pcm_len >= MIMO_TTS_PCM_FLUSH)
    {
      mimo_tts_pcm_deliver(st, 1);
    }

  return 0;
}

/* 逐字符喂 base64，凑满 4 字符就解一组，余数留在 carry 里。 */
static int mimo_tts_b64_feed(mimo_pcm_stream_t* st, const char* s, size_t len)
{
  size_t i;
  unsigned char out[3];

  for (i = 0; i < len; i++)
    {
      char c = s[i];

      if (c == '\r' || c == '\n' || c == ' ' || c == '\t')
        {
          continue;
        }
      st->b64_carry[st->b64_carry_len++] = c;
      if (st->b64_carry_len < 4)
        {
          continue;
        }

      st->b64_carry_len = 0;
      st->audio_b64_chars += 4;

      {
        int n = mimo_tts_b64_group(st->b64_carry, out);

        if (n < 0)
          {
            return -EPROTO;
          }
        if (n <= 0)
          {
            continue;
          }
        if (st->first_pcm_ms < 0)
          {
            st->first_pcm_ms = mimo_tts_elapsed_ms(&st->t0);
          }
        if (mimo_tts_pcm_append(st, out, (size_t)n) != 0)
          {
            return -ENOMEM;
          }
      }
    }

  return 0;
}

/* 在流式响应里找音频 base64：官方形状是 choices[0].delta.audio.data；
 * 顺手兼容 message.audio.data / choices[0].audio.data / 顶层 audio.data，
 * 避免网关版本差异直接把流式判死。 */
static const char* mimo_tts_audio_b64(cJSON* root)
{
  cJSON* choices = cJSON_GetObjectItem(root, "choices");
  cJSON* au;
  cJSON* data;

  if (choices != NULL && cJSON_IsArray(choices))
    {
      cJSON* ch = cJSON_GetArrayItem(choices, 0);

      if (ch != NULL)
        {
          cJSON* delta = cJSON_GetObjectItem(ch, "delta");

          if (delta != NULL)
            {
              au = cJSON_GetObjectItem(delta, "audio");
              data = (au != NULL) ? cJSON_GetObjectItem(au, "data") : NULL;
              if (data != NULL && cJSON_IsString(data)
                  && data->valuestring != NULL && data->valuestring[0] != '\0')
                {
                  return data->valuestring;
                }
            }

          {
            cJSON* message = cJSON_GetObjectItem(ch, "message");

            if (message != NULL)
              {
                au = cJSON_GetObjectItem(message, "audio");
                data = (au != NULL) ? cJSON_GetObjectItem(au, "data") : NULL;
                if (data != NULL && cJSON_IsString(data)
                    && data->valuestring != NULL
                    && data->valuestring[0] != '\0')
                  {
                    return data->valuestring;
                  }
              }
          }

          au = cJSON_GetObjectItem(ch, "audio");
          data = (au != NULL) ? cJSON_GetObjectItem(au, "data") : NULL;
          if (data != NULL && cJSON_IsString(data)
              && data->valuestring != NULL && data->valuestring[0] != '\0')
            {
              return data->valuestring;
            }
        }
    }

  au = cJSON_GetObjectItem(root, "audio");
  data = (au != NULL) ? cJSON_GetObjectItem(au, "data") : NULL;
  if (data != NULL && cJSON_IsString(data)
      && data->valuestring != NULL && data->valuestring[0] != '\0')
    {
      return data->valuestring;
    }

  return NULL;
}

/* 处理一整行 SSE（不含 '\n'；行尾 '\r' 已剥）。 */
static void mimo_tts_sse_line(mimo_pcm_stream_t* st)
{
  char* line = st->line;
  size_t len = st->line_len;
  char* p;
  cJSON* root;
  const char* b64;

  st->line_len = 0;
  st->sse_lines++;
  if (len > 0 && line[len - 1] == '\r')
    {
      len--;
    }
  line[len] = '\0';

  if (len == 0 || line[0] == ':')
    {
      return;   /* 空行 / SSE 注释 */
    }
  if (strncmp(line, "data:", 5) != 0)
    {
      return;   /* event:/id:/retry: 等字段忽略 */
    }

  p = line + 5;
  while (*p == ' ')
    {
      p++;
    }

  if (strcmp(p, "[DONE]") == 0)
    {
      st->saw_done = 1;
      return;
    }

  root = cJSON_Parse(p);
  if (root == NULL)
    {
      st->parse_errors++;
      return;
    }

  b64 = mimo_tts_audio_b64(root);
  if (b64 != NULL)
    {
      st->audio_deltas++;
      if (mimo_tts_b64_feed(st, b64, strlen(b64)) != 0)
        {
          if (!st->failed)
            {
              st->failed = 1;
              st->fail_err = -EPROTO;
            }
        }
    }

  cJSON_Delete(root);
}

/* vela_https_post_json_stream 的 body 回调：按行拼、逐行喂。 */
static int mimo_tts_stream_body_cb(const char* data, size_t len, void* arg)
{
  mimo_pcm_stream_t* st = (mimo_pcm_stream_t*)arg;
  size_t i;

  for (i = 0; i < len; i++)
    {
      char c = data[i];

      if (c == '\n')
        {
          st->line[st->line_len] = '\0';
          mimo_tts_sse_line(st);
          if (st->failed)
            {
              return st->fail_err != 0 ? st->fail_err : -EPROTO;
            }
          if (st->pcm_bytes > MIMO_TTS_PCM_MAX_BYTES)
            {
              return 1;   /* 提前结束（视为成功，已播的音频保留） */
            }
        }
      else
        {
          if (st->line_len + 1 >= st->line_cap)
            {
              size_t ncap = st->line_cap * 2u;
              char* np;

              if (ncap > MIMO_TTS_SSE_LINE_MAX)
                {
                  ncap = MIMO_TTS_SSE_LINE_MAX;
                }
              if (st->line_len + 1 >= ncap)
                {
                  return -EOVERFLOW;   /* 单行超限：回退非流式 */
                }
              np = (char*)realloc(st->line, ncap);
              if (np == NULL)
                {
                  return -ENOMEM;
                }
              st->line = np;
              st->line_cap = ncap;
            }
          st->line[st->line_len++] = c;
        }
    }

  return 0;
}

/* 组请求 + 发起流式合成。返回 0 表示已把 PCM 交给回调；负值交给上层回退。 */
static int mimo_tts_synthesize_pcm16(const char* text,
                                     struct mimo_tts_sink_s* sink)
{
  cJSON* root = NULL;
  char*  body = NULL;
  vela_header_t hdrs[2];
  mimo_pcm_stream_t st;
  int status;
  int ret = -EIO;

  memset(&st, 0, sizeof(st));
  st.sink = sink;
  st.first_pcm_ms = -1;
  st.first_audio_ms = -1;
  st.prebuffer_bytes = (size_t)MIMO_TTS_SAMPLE_RATE * 2u
                     * (size_t)MIMO_TTS_PREBUFFER_MS / 1000u;
  st.pcm_cap = st.prebuffer_bytes + 4096u;

  root = cJSON_CreateObject();
  if (root == NULL)
    {
      return -ENOMEM;
    }

  cJSON_AddStringToObject(root, "model", MIMO_TTS_MODEL);

  {
    cJSON* msgs = cJSON_CreateArray();
    cJSON* msg  = cJSON_CreateObject();

    cJSON_AddStringToObject(msg, "role", "assistant");
    cJSON_AddStringToObject(msg, "content", text);
    cJSON_AddItemToArray(msgs, msg);
    cJSON_AddItemToObject(root, "messages", msgs);
  }

  {
    cJSON* audio = cJSON_CreateObject();

    cJSON_AddStringToObject(audio, "format", "pcm16");
    cJSON_AddStringToObject(audio, "voice", MIMO_TTS_VOICE);
    cJSON_AddItemToObject(root, "audio", audio);
  }

  cJSON_AddBoolToObject(root, "stream", 1);

  body = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  root = NULL;
  if (body == NULL)
    {
      return -ENOMEM;
    }

  st.line_cap = MIMO_TTS_SSE_LINE_INIT;
  st.line = (char*)malloc(st.line_cap);
  st.pcm = (unsigned char*)malloc(st.pcm_cap);
  if (st.line == NULL || st.pcm == NULL)
    {
      ret = -ENOMEM;
      goto out;
    }
  st.line[0] = '\0';

  hdrs[0].name  = "Authorization";
  hdrs[0].value = s_auth;
  hdrs[1].name  = NULL;
  hdrs[1].value = NULL;

  clock_gettime(CLOCK_MONOTONIC, &st.t0);

  syslog(LOG_INFO,
         "[%s] pcm16 stream POST (%u body bytes, text %u chars, prebuffer %u B)\n",
         MIMO_TTS_TAG, (unsigned)strlen(body), (unsigned)strlen(text),
         (unsigned)st.prebuffer_bytes);

  status = vela_https_post_json_stream(MIMO_API_HOST, MIMO_API_PORT,
                                       MIMO_API_PATH, hdrs, body,
                                       mimo_tts_stream_body_cb, &st,
                                       (uint32_t)MIMO_TTS_STREAM_TIMEOUT_MS);
  free(body);
  body = NULL;

  /* 收尾：把不足 FLUSH / 不足预灌的尾巴也发出去（偶数对齐）。
   * 只要已经播出过音频，就按成功处理：宁可少念几个字，也不要重放整句。 */
  if (st.playback_started || st.pcm_len > 0)
    {
      mimo_tts_pcm_deliver(&st, 1);
    }

  if (status == 200 && st.pcm_bytes > 0)
    {
      long total_ms = mimo_tts_elapsed_ms(&st.t0);

      syslog(LOG_INFO,
             "[%s] pcm16 stream done: status=%d sse=%d audio_deltas=%d "
             "parse_err=%d done=%d b64=%u pcm=%u chunks=%d "
             "first_pcm=%ldms first_audio=%ldms total=%ldms\n",
             MIMO_TTS_TAG, status, st.sse_lines, st.audio_deltas,
             st.parse_errors, st.saw_done,
             (unsigned)st.audio_b64_chars, (unsigned)st.pcm_bytes,
             st.chunks, st.first_pcm_ms, st.first_audio_ms, total_ms);
    }
  else if (status == 200)
    {
      syslog(LOG_WARNING,
             "[%s] pcm16 stream no audio: sse=%d audio_deltas=%d parse_err=%d "
             "(server may not support stream -> fallback)\n",
             MIMO_TTS_TAG, st.sse_lines, st.audio_deltas, st.parse_errors);
    }
  else
    {
      syslog(LOG_WARNING,
             "[%s] pcm16 stream failed: status=%d sse=%d audio_deltas=%d "
             "parse_err=%d pcm=%u failed=%d\n",
             MIMO_TTS_TAG, status, st.sse_lines, st.audio_deltas,
             st.parse_errors, (unsigned)st.pcm_bytes, st.failed);
    }

  if (st.pcm_bytes > 0)
    {
      /* 已播出过 PCM：可播即播，不触发 MP3 重放。 */
      ret = 0;
    }
  else if (status != 200)
    {
      ret = (status < 0) ? status : -EIO;
    }
  else if (st.failed)
    {
      ret = (st.fail_err != 0) ? st.fail_err : -EPROTO;
    }
  else
    {
      ret = -ENODATA;   /* 200 但一个 audio delta 都没有（服务端没走流式） */
    }

out:
  free(st.line);
  free(st.pcm);
  free(body);
  return ret;
}

#endif /* MIMO_TTS_PCM16_STREAM */

/* 对外入口：优先 PCM16 流式；不可行时回退原来的整段 MP3。 */
int mimo_tts_synthesize_stream(const char* text,
                               voice_tts_chunk_cb cb, void* user_data)
{
  struct mimo_tts_sink_s sink;
  int ret = -EIO;

  if (!s_ready && mimo_tts_init() != 0)
    {
      return -ENOENT;
    }

  if (text == NULL || text[0] == '\0')
    {
      return -EINVAL;
    }

  memset(&sink, 0, sizeof(sink));
  sink.cb = cb;
  sink.user = user_data;

#if MIMO_TTS_PCM16_STREAM
  ret = mimo_tts_synthesize_pcm16(text, &sink);

  if (ret == 0)
    {
      if (cb != NULL)
        {
          cb(NULL, 0, 1, user_data);     /* 收尾：本句结束 */
        }
      return 0;
    }

  if (sink.pcm_bytes > 0)
    {
      /* 部分音频已经播出过：与 voice_tts 的 "keeping played audio" 同策略，
       * 视作成功，避免同一句从头再念一遍。 */
      syslog(LOG_WARNING,
             "[%s] pcm16 stream partial: keep %u played PCM bytes (ret=%d)\n",
             MIMO_TTS_TAG, (unsigned)sink.pcm_bytes, ret);
      if (cb != NULL)
        {
          cb(NULL, 0, 1, user_data);
        }
      return 0;
    }

  syslog(LOG_WARNING,
         "[%s] pcm16 stream unavailable (%d), fallback to non-stream mp3\n",
         MIMO_TTS_TAG, ret);
#else
  (void)ret;
#endif

  return mimo_tts_synthesize_mp3(text, cb, user_data);
}

/* ── 一次性接口: voice_tts_register() 要求 ops->synthesize 非空 ──
 * 用流式路径把整句 PCM 收进缓冲即可; 上层真正播报走 synthesize_stream,
 * 这个接口主要给 voice_test_tts 和兜底链第 5 步用。 */
struct mimo_tts_acc_s
{
  unsigned char* buf;
  size_t         len;
  size_t         cap;
  int            oom;
};

static void mimo_tts_acc_cb(const unsigned char* pcm, size_t len, int is_last,
                            void* user_data)
{
  struct mimo_tts_acc_s* acc = (struct mimo_tts_acc_s*)user_data;

  (void)is_last;

  if (acc->oom || pcm == NULL || len == 0)
    {
      return;
    }

  if (acc->len + len > acc->cap)
    {
      size_t ncap = acc->cap ? acc->cap * 2u : (32u * 1024u);
      unsigned char* np;

      if (ncap < acc->len + len)
        {
          ncap = acc->len + len;
        }

      np = (unsigned char*)realloc(acc->buf, ncap);
      if (np == NULL)
        {
          acc->oom = 1;
          return;
        }

      acc->buf = np;
      acc->cap = ncap;
    }

  memcpy(acc->buf + acc->len, pcm, len);
  acc->len += len;
}

static int mimo_tts_synthesize(const char* text,
                               unsigned char* pcm_out, size_t pcm_cap,
                               size_t* pcm_len)
{
  struct mimo_tts_acc_s acc;
  int ret;

  if (pcm_out == NULL || pcm_len == NULL || pcm_cap == 0)
    {
      return -EINVAL;
    }

  memset(&acc, 0, sizeof(acc));

  ret = mimo_tts_synthesize_stream(text, mimo_tts_acc_cb, &acc);
  if (ret != 0 || acc.oom || acc.buf == NULL || acc.len == 0)
    {
      free(acc.buf);
      return (ret != 0) ? ret : -ENODATA;
    }

  /* v125 P0-B（2026-11-03）：**这里就是真机 -28 的判定点**。
   * 旧代码：acc.len > pcm_cap 就 free 整句返回 -ENOSPC。真机一次 speak 中
   * MiMo 已经解出 164 736 B PCM（3.4 s 音频），只因超过一次性缓冲
   * AGENT_VOICE_PCM_BUF_SIZE = 128 KB，整句被丢弃 → 播放器 0 字节播出、
   * 上层判 TTS 失败（serial_perf_session.log:1938-1966）。
   * 现在改成"可播即播"：截取缓冲放得下的前缀（16-bit 对齐）按成功返回；
   * 上层 voice_channel 会把这段 PCM 真的写进播放器。流式路径
   * (mimo_tts_synthesize_stream) 不受此限，逐块解码直接播。 */
  if (acc.len > pcm_cap)
    {
      size_t playable = pcm_cap & ~(size_t)1; /* 16-bit PCM：保持偶数字节 */

      if (playable == 0)
        {
          free(acc.buf);
          return -ENOSPC;
        }

      syslog(LOG_WARNING,
             "[%s] pcm %u B > one-shot cap %u B: play playable prefix %u B "
             "(truncated %u B)\n",
             MIMO_TTS_TAG, (unsigned)acc.len, (unsigned)pcm_cap,
             (unsigned)playable, (unsigned)(acc.len - playable));

      memcpy(pcm_out, acc.buf, playable);
      *pcm_len = playable;
      free(acc.buf);
      return 0;
    }

  memcpy(pcm_out, acc.buf, acc.len);
  *pcm_len = acc.len;
  free(acc.buf);

  return 0;
}

static const voice_tts_ops_t s_mimo_tts_ops =
{
  .name              = "mimo",
  .init              = mimo_tts_init,
  .synthesize        = mimo_tts_synthesize,
  .deinit            = NULL,
  .synthesize_stream = mimo_tts_synthesize_stream,
  .sample_rate       = 24000,
};

int mimo_tts_register(void)
{
  return voice_tts_register(&s_mimo_tts_ops);
}
