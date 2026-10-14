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
 * pcm_media_recorder.c —— 直接用 NuttX 音频驱动录制
 *
 * 为什么需要这个文件(2026-10-14 真机定位):
 *   本固件**没有**把 frameworks/multimedia/media 编进来, 于是 src/stubs.c 里
 *   那组"永远失败"的 weak 桩成了 media_recorder_* 的唯一实现:
 *       nm nuttx | grep media_recorder_   =>  全部是 W(weak)
 *   后果是 audio_capture_open() 的 media_recorder 分支必然失败:
 *       [audio_cap] media_recorder_open failed, errno=11
 *       [voice] capture open failed
 *   也就是说 —— **实时语音采集在这块板上从来没工作过**, 之前能用的只有
 *   "先用 audio_rec_test 录成文件, 再 voice_test_asr 识别"这条离线路径。
 *
 * 采集序列照抄已被真机验证的
 *   contest.../app/ai_agent/audio/audio_rec_test_main.c
 * (open(O_RDWR) -> RESERVE -> CONFIGURE(16k/16bit/mono) -> mq_open ->
 *  REGISTERMQ -> GETBUFFERINFO -> ALLOCBUFFER xN + ENQUEUE ->
 *  START -> mq_receive(DEQUEUE) -> 取 curbyte -> 重新 ENQUEUE)
 *
 * 播放侧的对等物是 src/voice/pcm_media_player.c(同样是直接驱动 + 强符号)。
 * 这里的符号与 stubs.c 的弱符号同名, 链接时强符号胜出, 桩自然被顶掉。
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <nuttx/audio/audio.h>

#define TAG          "pcm_rec"
#define REC_DEVPATH  "/dev/audio/pcm0c"
#define REC_MQNAME   "/pcm_rec_mq"
#define REC_RATE     16000
#define REC_CHANNELS 1
#define REC_BPSAMP   16
#define REC_BUFBYTES 4096
#define REC_NBUFS    8

struct pcm_rec_s
{
  int                  afd;
  mqd_t                mq;
  int                  mq_ok;
  int                  started;
  struct ap_buffer_s*  apbs[REC_NBUFS];
  int                  nbufs;

  /* 一次 DEQUEUE 拿到的数据可能多于调用方要的, 余下的先存这里 */
  unsigned char        carry[REC_BUFBYTES];
  size_t               carry_len;
  size_t               carry_off;
};

static int rec_enqueue(struct pcm_rec_s* r, struct ap_buffer_s* apb)
{
  struct audio_buf_desc_s desc;

  memset(&desc, 0, sizeof(desc));
  apb->nbytes   = apb->nmaxbytes;
  desc.numbytes = apb->nbytes;
  desc.u.buffer = apb;
  return ioctl(r->afd, AUDIOIOC_ENQUEUEBUFFER, (uintptr_t)&desc);
}

/* 解析 audio_capture.c 传来的选项串:
 *   format=s16le:sample_rate=16000:ch_layout=mono */
static void rec_parse_opts(const char* opts, int* rate, int* channels)
{
  const char* p;

  *rate = REC_RATE;
  *channels = REC_CHANNELS;

  if (opts == NULL)
    {
      return;
    }

  p = strstr(opts, "sample_rate=");
  if (p != NULL)
    {
      int v = atoi(p + 12);

      if (v > 0)
        {
          *rate = v;
        }
    }

  p = strstr(opts, "ch_layout=");
  if (p != NULL)
    {
      if (strncmp(p + 10, "stereo", 6) == 0)
        {
          *channels = 2;
        }
      else if (strncmp(p + 10, "mono", 4) == 0)
        {
          *channels = 1;
        }
    }
}

/* ── 对外的强符号实现 ─────────────────────────────────────────────────── */

void* media_recorder_open(const char* params)
{
  struct pcm_rec_s* r;

  (void)params;

  r = (struct pcm_rec_s*)calloc(1, sizeof(*r));
  if (r == NULL)
    {
      return NULL;
    }

  r->afd = open(REC_DEVPATH, O_RDWR);
  r->mq = (mqd_t)-1;

  if (r->afd < 0)
    {
      syslog(LOG_ERR, "[%s] open %s failed: %d\n", TAG, REC_DEVPATH, errno);
      free(r);
      return NULL;
    }

  if (ioctl(r->afd, AUDIOIOC_RESERVE) < 0)
    {
      syslog(LOG_ERR, "[%s] RESERVE failed: %d\n", TAG, errno);
      close(r->afd);
      free(r);
      return NULL;
    }

  syslog(LOG_INFO, "[%s] opened %s\n", TAG, REC_DEVPATH);
  return r;
}

int media_recorder_prepare(void* handle, const char* url, const char* opts)
{
  struct pcm_rec_s* r = (struct pcm_rec_s*)handle;
  struct audio_caps_desc_s cap_desc;
  struct ap_buffer_info_s binfo;
  struct mq_attr attr;
  int rate;
  int channels;
  int i;

  (void)url;

  if (r == NULL)
    {
      return -EINVAL;
    }

  /* 已经 prepare 过就先不是错误: 直接复用 */
  if (r->nbufs > 0)
    {
      return 0;
    }

  rec_parse_opts(opts, &rate, &channels);

  memset(&cap_desc, 0, sizeof(cap_desc));
  cap_desc.caps.ac_len            = sizeof(cap_desc.caps);
  cap_desc.caps.ac_type           = AUDIO_TYPE_INPUT;
  cap_desc.caps.ac_channels       = channels;
  cap_desc.caps.ac_controls.hw[0] = rate;
  cap_desc.caps.ac_controls.b[2]  = REC_BPSAMP;
  cap_desc.caps.ac_subtype        = AUDIO_FMT_PCM;

  if (ioctl(r->afd, AUDIOIOC_CONFIGURE, (uintptr_t)&cap_desc) < 0)
    {
      syslog(LOG_ERR, "[%s] CONFIGURE failed: %d\n", TAG, errno);
      return -errno;
    }

  /* mq 必须是 O_RDWR: 内核侧要往里面 file_mq_send */
  attr.mq_maxmsg  = 16;
  attr.mq_msgsize = sizeof(struct audio_msg_s);
  attr.mq_flags   = 0;

  r->mq = mq_open(REC_MQNAME, O_CREAT | O_RDWR, 0644, &attr);
  if (r->mq == (mqd_t)-1)
    {
      syslog(LOG_ERR, "[%s] mq_open failed: %d\n", TAG, errno);
      return -errno;
    }

  if (ioctl(r->afd, AUDIOIOC_REGISTERMQ, (uintptr_t)r->mq) < 0)
    {
      syslog(LOG_ERR, "[%s] REGISTERMQ failed: %d\n", TAG, errno);
      return -errno;
    }

  r->mq_ok = 1;

  memset(&binfo, 0, sizeof(binfo));
  if (ioctl(r->afd, AUDIOIOC_GETBUFFERINFO, (uintptr_t)&binfo) != OK)
    {
      binfo.buffer_size = REC_BUFBYTES;
      binfo.nbuffers    = REC_NBUFS;
    }

  if (binfo.buffer_size <= 0 || binfo.buffer_size > REC_BUFBYTES)
    {
      binfo.buffer_size = REC_BUFBYTES;
    }

  if (binfo.nbuffers <= 0 || binfo.nbuffers > REC_NBUFS)
    {
      binfo.nbuffers = REC_NBUFS;
    }

  for (i = 0; i < binfo.nbuffers; i++)
    {
      struct audio_buf_desc_s desc;

      memset(&desc, 0, sizeof(desc));
      desc.numbytes  = binfo.buffer_size;
      desc.u.pbuffer = &r->apbs[i];

      if (ioctl(r->afd, AUDIOIOC_ALLOCBUFFER, (uintptr_t)&desc)
          != sizeof(desc))
        {
          syslog(LOG_ERR, "[%s] ALLOCBUFFER %d failed\n", TAG, i);
          return -ENOMEM;
        }

      if (r->apbs[i] != NULL)
        {
          memset(r->apbs[i]->samp, 0, r->apbs[i]->nmaxbytes);
          r->apbs[i]->nbytes  = 0;
          r->apbs[i]->curbyte = 0;
        }

      r->nbufs++;

      if (rec_enqueue(r, r->apbs[i]) < 0)
        {
          syslog(LOG_ERR, "[%s] ENQUEUEBUFFER %d failed: %d\n", TAG, i,
                 errno);
          return -errno;
        }
    }

  syslog(LOG_INFO, "[%s] prepared %d bufs x %d bytes @ %dHz %uch\n",
         TAG, r->nbufs, (int)binfo.buffer_size, rate, channels);
  return 0;
}

int media_recorder_start(void* handle)
{
  struct pcm_rec_s* r = (struct pcm_rec_s*)handle;

  if (r == NULL)
    {
      return -EINVAL;
    }

  if (ioctl(r->afd, AUDIOIOC_START) < 0)
    {
      syslog(LOG_ERR, "[%s] START failed: %d\n", TAG, errno);
      return -errno;
    }

  r->started = 1;
  syslog(LOG_INFO, "[%s] started\n", TAG);
  return 0;
}

ssize_t media_recorder_read_data(void* handle, void* buf, size_t len)
{
  struct pcm_rec_s* r = (struct pcm_rec_s*)handle;
  struct audio_msg_s msg;
  struct timespec ts;
  size_t copied = 0;
  unsigned char* out = (unsigned char*)buf;

  if (r == NULL || buf == NULL || len == 0)
    {
      return -EINVAL;
    }

  /* 先把上次剩下的给出去 */
  if (r->carry_off < r->carry_len)
    {
      size_t avail = r->carry_len - r->carry_off;
      size_t take = avail < len ? avail : len;

      memcpy(out, r->carry + r->carry_off, take);
      r->carry_off += take;
      return (ssize_t)take;
    }

  for (;;)
    {
      ssize_t n;

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_nsec += 200000000L;                 /* 200ms */
      if (ts.tv_nsec >= 1000000000L)
        {
          ts.tv_nsec -= 1000000000L;
          ts.tv_sec++;
        }

      n = mq_timedreceive(r->mq, (char*)&msg, sizeof(msg), NULL, &ts);
      if (n < 0)
        {
          if (errno == ETIMEDOUT)
            {
              /* ★ 超时必须返回 -EAGAIN, **不能返回 0**: 调用方
               *   (recording_thread)把 EAGAIN 当作"暂时没数据"重试, 而把
               *   n <= 0 当作致命错误直接结束录制。
               *   真机 2026-10-14 就是踩了这个: 采到 122 块后一次超时让录制
               *   线程以 abnormal 退出 → 流被 abort → 整段录音白采
               *   (日志 "[voice] recorded 0 bytes")。
               *   超时取 200ms 而非 2s: 停止录制的响应不会拖沓。 */
              return copied > 0 ? (ssize_t)copied : -EAGAIN;
            }

          return -errno;
        }

      if (msg.msg_id != AUDIO_MSG_DEQUEUE || msg.u.ptr == NULL)
        {
          continue;
        }

      {
        struct ap_buffer_s* apb = msg.u.ptr;
        size_t avail = (size_t)apb->curbyte;

        if (avail > (size_t)apb->nmaxbytes)
          {
            avail = (size_t)apb->nmaxbytes;
          }

        if (avail > 0)
          {
            size_t take = avail < len ? avail : len;

            memcpy(out, apb->samp, take);
            copied = take;

            if (take < avail)
              {
                /* 装不下, 余下的留到下次 */
                memcpy(r->carry, (unsigned char*)apb->samp + take,
                       avail - take);
                r->carry_len = avail - take;
                r->carry_off = 0;
              }
          }
        else
          {
            r->carry_len = 0;
            r->carry_off = 0;
          }

        /* 标记已消费完并还回驱动 */
        apb->nbytes = apb->curbyte;
        if (rec_enqueue(r, apb) < 0)
          {
            syslog(LOG_ERR, "[%s] re-enqueue failed: %d\n", TAG, errno);
          }
      }

      if (copied > 0)
        {
          return (ssize_t)copied;
        }

      /* 拿到的是空缓冲, 继续等 */
    }
}

int media_recorder_stop(void* handle)
{
  struct pcm_rec_s* r = (struct pcm_rec_s*)handle;

  if (r == NULL)
    {
      return -EINVAL;
    }

  if (r->started)
    {
      ioctl(r->afd, AUDIOIOC_STOP, 0);
      r->started = 0;
    }

  return 0;
}

int media_recorder_close(void* handle)
{
  struct pcm_rec_s* r = (struct pcm_rec_s*)handle;
  int i;

  if (r == NULL)
    {
      return -EINVAL;
    }

  if (r->started)
    {
      ioctl(r->afd, AUDIOIOC_STOP, 0);
    }

  for (i = 0; i < r->nbufs && i < REC_NBUFS; i++)
    {
      if (r->apbs[i] != NULL)
        {
          struct audio_buf_desc_s desc;

          memset(&desc, 0, sizeof(desc));
          desc.u.buffer = r->apbs[i];
          ioctl(r->afd, AUDIOIOC_FREEBUFFER, (uintptr_t)&desc);
          r->apbs[i] = NULL;
        }
    }

  if (r->mq_ok)
    {
      ioctl(r->afd, AUDIOIOC_UNREGISTERMQ);
      r->mq_ok = 0;
    }

  if (r->mq != (mqd_t)-1)
    {
      mq_close(r->mq);
      mq_unlink(REC_MQNAME);
      r->mq = (mqd_t)-1;
    }

  if (r->afd >= 0)
    {
      ioctl(r->afd, AUDIOIOC_RELEASE);
      close(r->afd);
      r->afd = -1;
    }

  free(r);
  return 0;
}
