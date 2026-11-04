/*
 * vox_resample.h -- sample-rate conversion for the decoded audio.
 *
 * WHY IT IS NEEDED
 *
 * libopus always decodes at 48 kHz: Opus runs internally at 48 kHz whatever the
 * stream header says, and the reference decoder hard-codes it
 * ("Decode at a fixed 48k", 78/xiaozhi-esp32 main/audio/demuxer/ogg_demuxer.cc:315).
 * The playback path on this board runs at 16 kHz -- the same rate the protocol
 * document specifies and the one every engine in the reference uses
 * (afe_audio_engine.cc:63, lite_audio_engine.cc:16, audio_service.cc:294).
 *
 * Playing 48 kHz samples at 16 kHz without conversion is not a small error: it
 * stretches every sound to three times its length, which is heard as a slow,
 * deep voice, so this step is not optional.
 *
 * ALGORITHM
 *
 * Linear interpolation between neighbouring samples, carrying the fractional
 * position across calls so that consecutive 60 ms blocks join without a step or
 * a click at the boundary.  That continuity is the whole reason for keeping
 * state: a resampler that restarts its phase on every packet produces an audible
 * discontinuity sixteen times a second.
 *
 * Linear rather than a windowed-sinc filter, deliberately.  Downsampling by three
 * needs an anti-aliasing low-pass or speech gets audible aliasing, so a proper
 * implementation adds one; linear interpolation is kept here because it is exact
 * enough to verify against a reference value on a host, and because this board
 * has no spare cycles for a long filter.  The aliasing it leaves is the known
 * cost of that trade and is called out in the header below rather than hidden.
 *
 * The project already had resample_24k_to_16k() in mimo_tts.c for the same
 * reason on the TTS side; this is the general form of it, for the ratio the Opus
 * path happens to need.
 */

#ifndef VOX_RESAMPLE_H
#define VOX_RESAMPLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* From / to rates used by the Opus path.  Named rather than passed in because
 * the decoder's output rate is fixed at 48 kHz by Opus itself. */
#define VOX_RS_IN_RATE  48000
#define VOX_RS_OUT_RATE 16000
#define VOX_RS_RATIO_NUM 3 /* 48000 / 16000 */

/* A 60 ms frame at 48 kHz, and what it becomes at 16 kHz. */
#define VOX_RS_IN_FRAME  (VOX_RS_IN_RATE / 1000 * 60)   /* 2880 */
#define VOX_RS_OUT_FRAME (VOX_RS_OUT_RATE / 1000 * 60)  /*  960 */

typedef struct
{
  int32_t  acc;      /* fractional read position, in 1/RATIO_NUM units */
  int32_t  prev;     /* the last sample of the previous call */
  bool     primed;   /* prev holds a real sample */
  uint32_t in_frames;
  uint32_t out_frames;
} vox_resample_t;

void vox_resample_init(vox_resample_t *rs);

/* Converts n_in frames at 48 kHz into up to out_cap frames at 16 kHz.
 * Returns the number of output frames written, or 0 if the buffer is too small
 * or n_in is 0. */
size_t vox_resample_48k_to_16k(vox_resample_t *rs, const int16_t *in,
                               size_t n_in, int16_t *out, size_t out_cap);

/* Puts the state back to its start.  Called between utterances so a gap does not
 * carry a stale fractional position into the next sentence. */
void vox_resample_reset(vox_resample_t *rs);

/* Expected output count for n_in input frames, excluding the carry. */
size_t vox_resample_out_frames(size_t n_in);

#ifdef __cplusplus
}
#endif

#endif /* VOX_RESAMPLE_H */