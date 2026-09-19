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

/* audio_playback.c - Streaming audio playback via direct ALSA (aw-alsa-lib)
 * or media_player buffer mode.
 *
 * On the Allwinner R528 (Gemini-S1) board the openvela media_player framework
 * is not available at runtime, so playback uses the chip's aw-tiny-alsa-lib
 * directly (gated behind CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT). When that option
 * is disabled (or ALSA open fails), we fall back to media_player. */

#include "voice/audio_playback.h"
#include "agent_config.h"

#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
#include <aw-alsa-lib/pcm.h>
#endif
#include <errno.h>
#include <media_player.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

static const char* TAG = "audio_pb";

#define PB_OPTIONS_LEN 128

enum audio_playback_backend {
    AUDIO_PLAYBACK_BACKEND_NONE = 0,
    AUDIO_PLAYBACK_BACKEND_ALSA,
    AUDIO_PLAYBACK_BACKEND_MEDIA_PLAYER,
};

struct audio_playback {
    enum audio_playback_backend backend;
    union {
#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
        snd_pcm_t* pcm;
#endif
        void* player;
    } handle;
    size_t total_written;
    int bytes_per_frame;
    volatile int stopped;
};

static audio_playback_t* s_active_pb;

#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT

static const char* resolve_alsa_name(const char* dev_path)
{
    if (!dev_path || dev_path[0] == '\0') {
        return "default";
    }

    if (strncmp(dev_path, "/dev/audio/", strlen("/dev/audio/")) == 0) {
        return "default";
    }

    return dev_path;
}

static int configure_alsa_playback(snd_pcm_t* pcm,
    unsigned int sample_rate, unsigned int channels)
{
    snd_pcm_hw_params_t* hw;
    snd_pcm_sw_params_t* sw;
    snd_pcm_uframes_t period_frames = sample_rate / 10;
    snd_pcm_uframes_t buffer_frames;
    snd_pcm_uframes_t boundary;
    int ret;

    if (period_frames == 0) {
        period_frames = 256;
    }

    buffer_frames = period_frames * 4;

    snd_pcm_hw_params_alloca(&hw);
    ret = snd_vela_pcm_hw_params_any(pcm, hw);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_hw_params_set_access(pcm, hw,
        SND_PCM_ACCESS_RW_INTERLEAVED);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S16_LE);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_hw_params_set_channels(pcm, hw, channels);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_hw_params_set_rate(pcm, hw, sample_rate, 0);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_hw_params_set_period_size(pcm, hw,
        period_frames, 0);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_hw_params_set_buffer_size(pcm, hw,
        buffer_frames);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_hw_params(pcm, hw);
    if (ret < 0) {
        return ret;
    }

    snd_pcm_sw_params_alloca(&sw);
    ret = snd_vela_pcm_sw_params_current(pcm, sw);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_sw_params_get_boundary(sw, &boundary);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_sw_params_set_start_threshold(pcm, sw,
        buffer_frames);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_sw_params_set_silence_size(pcm, sw, boundary);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_sw_params_set_stop_threshold(pcm, sw,
        buffer_frames);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_sw_params_set_avail_min(pcm, sw, period_frames);
    if (ret < 0) {
        return ret;
    }

    return snd_vela_pcm_sw_params(pcm, sw);
}

static int open_alsa_playback(audio_playback_t* pb,
    const char* dev_path, unsigned int sample_rate,
    unsigned int channels, unsigned int bits_per_sample)
{
    (void)bits_per_sample; /* 16-bit only for now */

    const char* name = resolve_alsa_name(dev_path);
    snd_pcm_t* pcm = NULL;

    int ret = snd_vela_pcm_open(&pcm, name,
        SND_VELA_PCM_STREAM_PLAYBACK, 0);

    if (ret < 0) {
        syslog(LOG_ERR, "[%s] ALSA open failed: %d\n", TAG, ret);
        return ret;
    }

    ret = configure_alsa_playback(pcm, sample_rate, channels);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] ALSA configure failed: %d\n", TAG, ret);
        snd_vela_pcm_close(pcm);
        return ret;
    }

    pb->backend = AUDIO_PLAYBACK_BACKEND_ALSA;
    pb->handle.pcm = pcm;
    pb->bytes_per_frame = (bits_per_sample / 8) * channels;

    syslog(LOG_INFO,
        "[%s] opened direct ALSA playback (%s, %uHz, %uch, %ubit)\n",
        TAG, name, sample_rate, channels, bits_per_sample);
    return 0;
}

#endif /* CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT */

static int open_media_player_playback(audio_playback_t* pb,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    void* player = media_player_open(MEDIA_STREAM_MUSIC);

    if (!player) {
        syslog(LOG_ERR, "[%s] media_player_open failed\n", TAG);
        return -EIO;
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
        return ret;
    }

    ret = media_player_start(player);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] start failed: %d\n", TAG, ret);
        media_player_close(player, 0);
        return ret;
    }

    pb->backend = AUDIO_PLAYBACK_BACKEND_MEDIA_PLAYER;
    pb->handle.player = player;
    pb->bytes_per_frame = (bits_per_sample / 8) * channels;

    syslog(LOG_INFO, "[%s] opened media player (%uHz %uch %ubit)\n",
        TAG, sample_rate, channels, bits_per_sample);
    return 0;
}

audio_playback_t* audio_playback_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    if (s_active_pb) {
        syslog(LOG_WARNING, "[%s] force closing stale player\n", TAG);
        audio_playback_close(s_active_pb);
        usleep(100000);
    }

    audio_playback_t* pb = calloc(1, sizeof(*pb));

    if (!pb) {
        return NULL;
    }

    int ret = -ENOTSUP;
#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
    ret = open_alsa_playback(pb, dev_path, sample_rate, channels,
        bits_per_sample);
    if (ret < 0) {
        syslog(LOG_WARNING,
            "[%s] direct ALSA playback unavailable (%d), falling back to media player\n",
            TAG, ret);
    }
#else
    (void)dev_path;
#endif

    if (ret < 0) {
        ret = open_media_player_playback(pb, sample_rate, channels,
            bits_per_sample);
        if (ret < 0) {
            free(pb);
            return NULL;
        }
    }

    s_active_pb = pb;

    syslog(LOG_INFO, "[%s] opened (%uHz %uch %ubit)\n",
        TAG, sample_rate, channels, bits_per_sample);
    return pb;
}

int audio_playback_write(audio_playback_t* pb, const void* buf, size_t len)
{
    if (!pb || !buf || len == 0) {
        return -EINVAL;
    }

    if (pb->stopped) {
        return -ECANCELED;
    }

#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
    if (pb->backend == AUDIO_PLAYBACK_BACKEND_ALSA) {
        snd_pcm_t* pcm = pb->handle.pcm;
        const unsigned char* src = buf;
        size_t written_bytes = 0;

        while (written_bytes < len) {
            snd_pcm_uframes_t frames = snd_vela_pcm_bytes_to_frames(pcm,
                len - written_bytes);
            snd_pcm_sframes_t n = snd_vela_pcm_writei(pcm, src, frames);

            if (n == -EAGAIN) {
                usleep(10 * 1000);
                continue;
            }

            if (n == -EPIPE || n == -ESTRPIPE) {
                snd_vela_pcm_prepare(pcm);
                continue;
            }

            if (n <= 0) {
                syslog(LOG_ERR, "[%s] ALSA write failed: %ld\n",
                    TAG, (long)n);
                return (n < 0) ? (int)n : -EIO;
            }

            size_t bytes = (size_t)n * (size_t)pb->bytes_per_frame;
            src += bytes;
            written_bytes += bytes;
        }

        pb->total_written += written_bytes;
        return (int)written_bytes;
    }
#endif

    ssize_t n = media_player_write_data(pb->handle.player, buf, len);

    if (n > 0) {
        pb->total_written += (size_t)n;
    }

    return (int)n;
}

void audio_playback_stop(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

    pb->stopped = 1;

#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
    if (pb->backend == AUDIO_PLAYBACK_BACKEND_ALSA) {
        if (pb->handle.pcm) {
            snd_vela_pcm_drop(pb->handle.pcm);
        }
        return;
    }
#endif

    if (pb->handle.player) {
        media_player_stop(pb->handle.player);
    }
}

void audio_playback_close(audio_playback_t* pb)
{
    if (!pb) {
        return;
    }

#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
    if (pb->backend == AUDIO_PLAYBACK_BACKEND_ALSA) {
        if (pb->handle.pcm) {
            if (!pb->stopped) {
                /* Block until all written samples have played out. */
                struct timespec d0, d1;
                clock_gettime(CLOCK_MONOTONIC, &d0);
                int drc = snd_vela_pcm_drain(pb->handle.pcm);
                clock_gettime(CLOCK_MONOTONIC, &d1);
                long dms = (d1.tv_sec - d0.tv_sec) * 1000
                    + (d1.tv_nsec - d0.tv_nsec) / 1000000;
                syslog(LOG_INFO,
                    "[%s] drain rc=%d took %ldms (written=%zu bytes)\n",
                    TAG, drc, dms, pb->total_written);
            }
            syslog(LOG_INFO, "[%s] closing ALSA (%zu bytes written)\n",
                TAG, pb->total_written);
            snd_vela_pcm_close(pb->handle.pcm);
        }
        s_active_pb = NULL;
        free(pb);
        return;
    }
#endif

    if (pb->handle.player) {
        syslog(LOG_INFO, "[%s] closing (%zu bytes written)\n",
            TAG, pb->total_written);
        media_player_stop(pb->handle.player);
        usleep(50 * 1000);
        media_player_close(pb->handle.player, 0);
    }

    s_active_pb = NULL;
    free(pb);
}
