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

/* audio_capture.c — Audio capture via direct ALSA or media_recorder fallback.
 *
 * The direct ALSA backend is gated behind CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
 * because it pulls in chip-specific ALSA headers (aw-alsa-lib) that are only
 * available on boards shipping that SDK. When the option is disabled (the
 * default), the agent uses the portable media_recorder backend that works on
 * any board with the Vela media framework. */

#include "voice/audio_capture.h"
#include "agent_config.h"

#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
#include <aw-alsa-lib/pcm.h>
#endif
#include <errno.h>
#include <media_recorder.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

static const char* TAG = "audio_cap";

#define CAP_OPTIONS_LEN 128
#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
#define CAP_ALSA_NAME_DEFAULT "default"
#endif

#ifdef CONFIG_AI_AGENT_AUDIO_CAPTURE_GAIN
#define AGENT_AUDIO_CAPTURE_GAIN CONFIG_AI_AGENT_AUDIO_CAPTURE_GAIN
#else
#define AGENT_AUDIO_CAPTURE_GAIN 6
#endif

enum audio_capture_backend {
    AUDIO_CAPTURE_BACKEND_NONE = 0,
    AUDIO_CAPTURE_BACKEND_ALSA,
    AUDIO_CAPTURE_BACKEND_MEDIA_RECORDER,
};

struct audio_capture {
    enum audio_capture_backend backend;
    union {
#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
        snd_pcm_t* pcm;
#endif
        void* recorder;
    } handle;
    unsigned int bits_per_sample;
    unsigned int requested_channels;
    unsigned int hw_channels;
    size_t out_frame_bytes;
    size_t hw_frame_bytes;
    unsigned char* scratch;
    size_t scratch_size;
    int started;
};

static audio_capture_t* s_active_capture;

static void apply_capture_gain(void* buf, size_t len)
{
#if AGENT_AUDIO_CAPTURE_GAIN > 1
    int16_t* samples = (int16_t*)buf;
    int count = (int)len / (int)sizeof(int16_t);

    for (int i = 0; i < count; i++) {
        int32_t v = (int32_t)samples[i] * AGENT_AUDIO_CAPTURE_GAIN;
        if (v > 32767) {
            v = 32767;
        } else if (v < -32768) {
            v = -32768;
        }
        samples[i] = (int16_t)v;
    }
#else
    (void)buf;
    (void)len;
#endif
}

#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
static const char* resolve_alsa_name(const char* dev_path)
{
    if (!dev_path || dev_path[0] == '\0') {
        return CAP_ALSA_NAME_DEFAULT;
    }

    if (strncmp(dev_path, "/dev/audio/", strlen("/dev/audio/")) == 0) {
        return CAP_ALSA_NAME_DEFAULT;
    }

    return dev_path;
}

static snd_pcm_format_t capture_format_from_bits(unsigned int bits_per_sample)
{
    switch (bits_per_sample) {
    case 16:
        return SND_PCM_FORMAT_S16_LE;
    case 24:
        return SND_PCM_FORMAT_S24_LE;
    default:
        return (snd_pcm_format_t)-1;
    }
}

static int configure_alsa_capture(snd_pcm_t* pcm, snd_pcm_format_t format,
    unsigned int sample_rate, unsigned int channels)
{
    snd_pcm_hw_params_t* hw;
    snd_pcm_sw_params_t* sw;
    snd_pcm_uframes_t period_frames = sample_rate / 10;
    snd_pcm_uframes_t buffer_frames;
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

    ret = snd_vela_pcm_hw_params_set_format(pcm, hw, format);
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

    ret = snd_vela_pcm_sw_params_set_start_threshold(pcm, sw, 1);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_sw_params_set_stop_threshold(pcm, sw, buffer_frames);
    if (ret < 0) {
        return ret;
    }

    ret = snd_vela_pcm_sw_params_set_avail_min(pcm, sw, period_frames);
    if (ret < 0) {
        return ret;
    }

    return snd_vela_pcm_sw_params(pcm, sw);
}

static int ensure_scratch_capacity(audio_capture_t* cap, size_t need)
{
    if (cap->scratch_size >= need) {
        return 0;
    }

    unsigned char* scratch = realloc(cap->scratch, need);
    if (!scratch) {
        return -ENOMEM;
    }

    cap->scratch = scratch;
    cap->scratch_size = need;
    return 0;
}

static int alsa_read_frames(audio_capture_t* cap, void* buf,
    snd_pcm_uframes_t frames)
{
    snd_pcm_uframes_t done = 0;
    unsigned char* dst = buf;

    while (done < frames) {
        snd_pcm_sframes_t ret = snd_vela_pcm_readi(cap->handle.pcm,
            dst + done * cap->hw_frame_bytes, frames - done);

        if (ret == -EAGAIN) {
            usleep(10 * 1000);
            continue;
        }

        if (ret == -EPIPE) {
            syslog(LOG_WARNING, "[%s] ALSA overrun, preparing capture\n",
                TAG);
            snd_vela_pcm_prepare(cap->handle.pcm);
            continue;
        }

        if (ret == -ESTRPIPE) {
            continue;
        }

        if (ret < 0) {
            return done > 0 ? (int)done : (int)ret;
        }

        done += (snd_pcm_uframes_t)ret;
    }

    return (int)done;
}

static void downmix_interleaved_s16_to_mono(audio_capture_t* cap,
    void* out_buf, const void* in_buf, size_t frames)
{
    int16_t* out = out_buf;
    const int16_t* in = in_buf;

    for (size_t i = 0; i < frames; i++) {
        int32_t mixed = 0;

        for (unsigned int ch = 0; ch < cap->hw_channels; ch++) {
            mixed += in[i * cap->hw_channels + ch];
        }

        out[i] = (int16_t)(mixed / (int32_t)cap->hw_channels);
    }
}

static int open_alsa_capture(audio_capture_t* cap, const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    const char* alsa_name = resolve_alsa_name(dev_path);
    snd_pcm_format_t format = capture_format_from_bits(bits_per_sample);
    unsigned int attempts[2];
    int attempt_count = 0;
    int last_ret = -ENODEV;

    if ((int)format < 0) {
        return -ENOTSUP;
    }

    attempts[attempt_count++] = channels;
    if (channels == 1) {
        attempts[attempt_count++] = 3;
    }

    for (int i = 0; i < attempt_count; i++) {
        snd_pcm_t* pcm = NULL;
        unsigned int hw_channels = attempts[i];
        int ret = snd_vela_pcm_open(&pcm, alsa_name,
            SND_VELA_PCM_STREAM_CAPTURE, 0);

        if (ret < 0) {
            last_ret = ret;
            continue;
        }

        ret = configure_alsa_capture(pcm, format, sample_rate, hw_channels);
        if (ret < 0) {
            snd_vela_pcm_close(pcm);
            last_ret = ret;
            continue;
        }

        cap->backend = AUDIO_CAPTURE_BACKEND_ALSA;
        cap->handle.pcm = pcm;
        cap->bits_per_sample = bits_per_sample;
        cap->requested_channels = channels;
        cap->hw_channels = hw_channels;
        cap->out_frame_bytes = (bits_per_sample / 8) * channels;
        cap->hw_frame_bytes = snd_vela_pcm_frames_to_bytes(pcm, 1);

        syslog(LOG_INFO,
            "[%s] opened direct ALSA capture (%s, %uHz, req=%uch, hw=%uch, %ubit)\n",
            TAG, alsa_name, sample_rate, channels, hw_channels,
            bits_per_sample);
        return 0;
    }

    return last_ret;
}

#endif /* CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT */

static int open_media_recorder_capture(audio_capture_t* cap,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    char opts[CAP_OPTIONS_LEN];
    int ret;

    cap->handle.recorder = media_recorder_open(MEDIA_SOURCE_MIC);
    if (!cap->handle.recorder) {
        syslog(LOG_ERR, "[%s] media_recorder_open failed, errno=%d\n",
            TAG, errno);
        return -errno;
    }

    snprintf(opts, sizeof(opts),
        "format=s%ule:sample_rate=%u:ch_layout=%s",
        bits_per_sample, sample_rate,
        (channels == 1) ? "mono" : "stereo");

    ret = media_recorder_prepare(cap->handle.recorder, NULL, opts);
    if (ret < 0) {
        syslog(LOG_ERR, "[%s] prepare failed: %d\n", TAG, ret);
        media_recorder_close(cap->handle.recorder);
        cap->handle.recorder = NULL;
        return ret;
    }

    cap->backend = AUDIO_CAPTURE_BACKEND_MEDIA_RECORDER;
    cap->bits_per_sample = bits_per_sample;
    cap->requested_channels = channels;
    cap->hw_channels = channels;
    cap->out_frame_bytes = (bits_per_sample / 8) * channels;
    cap->hw_frame_bytes = cap->out_frame_bytes;

    syslog(LOG_INFO, "[%s] opened media recorder (%uHz %uch %ubit)\n",
        TAG, sample_rate, channels, bits_per_sample);
    return 0;
}

audio_capture_t* audio_capture_open(const char* dev_path,
    unsigned int sample_rate, unsigned int channels,
    unsigned int bits_per_sample)
{
    if (s_active_capture) {
        syslog(LOG_WARNING, "[%s] force closing stale capture\n", TAG);
        audio_capture_close(s_active_capture);
        usleep(100000);
    }

    audio_capture_t* cap = calloc(1, sizeof(*cap));
    if (!cap) {
        return NULL;
    }

    int ret = -ENOTSUP;
#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
    ret = open_alsa_capture(cap, dev_path, sample_rate, channels,
        bits_per_sample);
    if (ret < 0) {
        syslog(LOG_WARNING,
            "[%s] direct ALSA capture unavailable (%d), falling back to media recorder\n",
            TAG, ret);
    }
#else
    (void)dev_path;
#endif

    if (ret < 0) {
        ret = open_media_recorder_capture(cap, sample_rate, channels,
            bits_per_sample);
        if (ret < 0) {
            free(cap);
            return NULL;
        }
    }

    s_active_capture = cap;
    return cap;
}

int audio_capture_start(audio_capture_t* cap)
{
    if (!cap) {
        return -EINVAL;
    }

    switch (cap->backend) {
    case AUDIO_CAPTURE_BACKEND_ALSA:
#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
        if (!cap->handle.pcm) {
            return -EINVAL;
        }

        if (cap->started) {
            return 0;
        }

        if (snd_vela_pcm_prepare(cap->handle.pcm) < 0) {
            syslog(LOG_ERR, "[%s] ALSA prepare failed\n", TAG);
            return -EIO;
        }

        cap->started = 1;
        syslog(LOG_INFO, "[%s] ALSA capture prepared\n", TAG);
        return 0;
#else
        return -ENOTSUP;
#endif

    case AUDIO_CAPTURE_BACKEND_MEDIA_RECORDER: {
        int ret;

        if (!cap->handle.recorder) {
            return -EINVAL;
        }

        ret = media_recorder_start(cap->handle.recorder);
        if (ret < 0) {
            syslog(LOG_ERR, "[%s] start failed: %d\n", TAG, ret);
            return ret;
        }

        syslog(LOG_INFO, "[%s] media recorder capture started\n", TAG);
        return 0;
    }

    default:
        return -EINVAL;
    }
}

int audio_capture_read(audio_capture_t* cap, void* buf, size_t len)
{
    if (!cap || !buf || len == 0) {
        return -EINVAL;
    }

    switch (cap->backend) {
    case AUDIO_CAPTURE_BACKEND_ALSA: {
#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
        snd_pcm_uframes_t frames;
        int nframes;
        size_t out_len;

        if (!cap->handle.pcm || cap->out_frame_bytes == 0) {
            return -EINVAL;
        }

        frames = len / cap->out_frame_bytes;
        if (frames == 0) {
            return -EINVAL;
        }

        if (cap->hw_channels == cap->requested_channels) {
            nframes = alsa_read_frames(cap, buf, frames);
            if (nframes <= 0) {
                return nframes;
            }

            out_len = (size_t)nframes * cap->out_frame_bytes;
            if (cap->bits_per_sample == 16) {
                apply_capture_gain(buf, out_len);
            }
            return (int)out_len;
        }

        if (cap->bits_per_sample != 16 || cap->requested_channels != 1) {
            return -ENOTSUP;
        }

        if (ensure_scratch_capacity(cap, frames * cap->hw_frame_bytes) < 0) {
            return -ENOMEM;
        }

        nframes = alsa_read_frames(cap, cap->scratch, frames);
        if (nframes <= 0) {
            return nframes;
        }

        downmix_interleaved_s16_to_mono(cap, buf, cap->scratch,
            (size_t)nframes);
        out_len = (size_t)nframes * cap->out_frame_bytes;
        apply_capture_gain(buf, out_len);
        return (int)out_len;
#else
        return -ENOTSUP;
#endif
    }

    case AUDIO_CAPTURE_BACKEND_MEDIA_RECORDER: {
        ssize_t n;

        if (!cap->handle.recorder) {
            return -EINVAL;
        }

        n = media_recorder_read_data(cap->handle.recorder, buf, len);
        if (n > 0 && cap->bits_per_sample == 16) {
            apply_capture_gain(buf, (size_t)n);
        }

        return (int)n;
    }

    default:
        return -EINVAL;
    }
}

int audio_capture_abort(audio_capture_t* cap)
{
    if (!cap) {
        return -EINVAL;
    }

    switch (cap->backend) {
    case AUDIO_CAPTURE_BACKEND_ALSA:
#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
        if (cap->handle.pcm && cap->started) {
            int ret = snd_vela_pcm_drop(cap->handle.pcm);
            cap->started = 0;
            return ret;
        }
        return 0;
#else
        return -ENOTSUP;
#endif

    case AUDIO_CAPTURE_BACKEND_MEDIA_RECORDER:
        if (cap->handle.recorder) {
            media_recorder_close_socket(cap->handle.recorder);
            return media_recorder_stop(cap->handle.recorder);
        }
        return 0;

    default:
        return -EINVAL;
    }
}

void audio_capture_close(audio_capture_t* cap)
{
    if (!cap) {
        return;
    }

    switch (cap->backend) {
    case AUDIO_CAPTURE_BACKEND_ALSA:
#ifdef CONFIG_AI_AGENT_AUDIO_ALSA_DIRECT
        if (cap->handle.pcm) {
            if (cap->started) {
                snd_vela_pcm_drop(cap->handle.pcm);
            }
            snd_vela_pcm_close(cap->handle.pcm);
            cap->handle.pcm = NULL;
        }
        free(cap->scratch);
        cap->scratch = NULL;
        cap->scratch_size = 0;
#endif
        break;

    case AUDIO_CAPTURE_BACKEND_MEDIA_RECORDER:
        if (cap->handle.recorder) {
            media_recorder_stop(cap->handle.recorder);
            usleep(50 * 1000);
            media_recorder_close(cap->handle.recorder);
            cap->handle.recorder = NULL;
        }
        break;

    default:
        break;
    }

    if (s_active_capture == cap) {
        s_active_capture = NULL;
    }

    free(cap);
    syslog(LOG_INFO, "[%s] closed\n", TAG);
}
