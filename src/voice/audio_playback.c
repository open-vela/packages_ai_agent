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

/* audio_playback.c - Streaming audio playback via media_player buffer mode. */

#include "voice/audio_playback.h"
#include "agent_config.h"

#include <errno.h>
#include <media_player.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "audio_pb";

#define PB_OPTIONS_LEN 128

static void* s_active_player;

struct audio_playback {
    void* player;
    size_t total_written;
    int bytes_per_frame;
    volatile int stopped;
};

audio_playback_t* audio_playback_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    (void)dev_path;

    if (s_active_player) {
        syslog(LOG_WARNING, "[%s] force closing stale player %p\n",
            TAG, s_active_player);
        media_player_stop(s_active_player);
        if (media_player_close(s_active_player, 0) == -EBUSY) {
            /* AUDIO-FIX：worker 仍活 -> pcm_media 已保留 fd/mq/APB（防 UAF），
             * 新的 media_player_open() 会在 worker 退出后回收。 */
            syslog(LOG_WARNING,
                "[%s] stale player deferred (worker alive), resources kept; "
                "reclaim on next open\n", TAG);
        }
        s_active_player = NULL;
        usleep(100000);
    }

    void* player = media_player_open(MEDIA_STREAM_MUSIC);
    if (!player) {
        /* AUDIO-FIX F2（RCA §4）：open 可能因"仍有活着的泄漏 worker"被
         * pcm_media 安全拒绝（防僵尸抢设备）—— 快速失败并给明确提示，
         * 不要进入 stop 长等待。 */
        syslog(LOG_ERR,
            "[%s] media_player_open failed (audio busy: leaked worker alive?) "
            "— fast-fail; retry after worker exits or reboot\n", TAG);
        return NULL;
    }

    char opts[PB_OPTIONS_LEN];
    snprintf(opts, sizeof(opts),
        "format=s%ule:sample_rate=%u:ch_layout=%s",
        bits_per_sample, sample_rate,
        (channels == 1) ? "mono" : "stereo");

    int ret = media_player_prepare(player, NULL, opts);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] prepare failed: %d\n", TAG, ret);
        media_player_close(player, 0);
        return NULL;
    }

    ret = media_player_start(player);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] start failed: %d\n", TAG, ret);
        media_player_close(player, 0);
        return NULL;
    }

    audio_playback_t* pb = calloc(1, sizeof(*pb));
    if (!pb) {
        media_player_close(player, 0);
        return NULL;
    }

    pb->player = player;
    pb->bytes_per_frame = (bits_per_sample / 8) * channels;
    s_active_player = player;

    syslog(LOG_INFO, "[%s] opened (%uHz %uch %ubit, player=%p)\n",
        TAG, sample_rate, channels, bits_per_sample, player);
    return pb;
}

int audio_playback_write(audio_playback_t* pb, const void* buf, size_t len)
{
    if (!pb || !pb->player || !buf || len == 0) {
        return -EINVAL;
    }

    if (pb->stopped) {
        return -ECANCELED;
    }

    ssize_t n = media_player_write_data(pb->player, buf, len);
    if (n > 0) {
        pb->total_written += (size_t)n;
    }

    return (int)n;
}

/* pcm_media_player.c 实现：用播放器自己的 fd 下发 AUDIO_FU_VOLUME。
 *
 * 注意头文件 <media_player.h> 里同名 API 声明的是 float 版本（框架原型），
 * 本仓 pcm_media_player.c 实际实现的是 int 版本（pcm_file_player.c 也按 int
 * 调用，AUDIO_FU_VOLUME 的 hw[0] 就是 int dB）。这里用 asm 标签起别名，
 * 避免与头文件原型冲突，同时保证按 int ABI 调用（ARM AAPCS：float 走 s0、
 * int 走 r1，混用会把垃圾音量下发到驱动）。 */
extern int audio_pb_set_volume_hw(void* player, int hw_volume)
    __asm__("media_player_set_volume");

/* P2A-感知增强（task-62）：会话内音量。
 * percent=100 -> 0 dB（基准最大），0 -> -36 dB；hw 音量只减不增，避免
 * >0 dB 增益削顶；且只用 pb 自己的 player fd，不二次 open。 */
int audio_playback_set_volume(audio_playback_t* pb, int percent)
{
    int hw;

    if (!pb || !pb->player) {
        return -EINVAL;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    hw = -36 + (percent * 36) / 100;
    if (hw > 0) {
        hw = 0;         /* 100% 就是 0 dB，绝不允许正向增益 */
    }

    if (audio_pb_set_volume_hw(pb->player, hw) < 0) {
        syslog(LOG_WARNING, "[%s] set volume %d%% (hw %d dB) failed\n",
            TAG, percent, hw);
        return -EIO;
    }

    syslog(LOG_INFO,
        "[%s] volume -> %d%% (hw %d dB, <=0dB), session player=%p\n",
        TAG, percent, hw, pb->player);
    return 0;
}

void audio_playback_stop(audio_playback_t* pb)
{
    if (pb) {
        pb->stopped = 1;
        if (pb->player) {
            media_player_stop(pb->player);
        }
    }
}

void audio_playback_close(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

    if (pb->player) {
        int ret;

        syslog(LOG_INFO, "[%s] closing (%zu bytes written, player=%p)\n",
            TAG, pb->total_written, pb->player);
        media_player_stop(pb->player);
        usleep(50 * 1000);
        ret = media_player_close(pb->player, 0);
        if (ret == -EBUSY) {
            /* AUDIO-FIX：worker 未停稳时 pcm_media 只登记泄漏、保留
             * fd/mq/APB（避免 UAF 污染下一次 TTS），worker 退出后下一次
             * media_player_open() 里 join+free。 */
            syslog(LOG_WARNING,
                "[%s] close deferred: player leaked safely (worker alive), "
                "reclaimed on next open\n", TAG);
        } else if (ret < 0) {
            syslog(LOG_WARNING, "[%s] media_player_close failed: %d\n",
                TAG, ret);
        }
        s_active_player = NULL;
    }

    free(pb);
}
