/*
 * vox_opus_dec.h -- Opus packet decoding to PCM, with a swappable backend.
 *
 * WHAT THIS ADDS
 *
 * vox_ogg_opus.c turns the server's binary frames into Opus packets.  This turns
 * packets into PCM the speaker can play.  Neither alone makes a sound: a stream
 * that is decoded but never resampled plays at the wrong speed, and one that is
 * resampled but never decoded plays noise.
 *
 * WHY THE BACKEND IS INJECTED
 *
 * libopus is a large C library with its own build.  It is built for this target
 * out of tree (see backend/README-opus.md) and linked in, but keeping it behind
 * an interface means:
 *
 *   - the resampling and buffering logic can be tested on a host, where no
 *     cross-compiled library exists;
 *   - a different decoder can be substituted if libopus does not fit;
 *   - the failure mode when no backend is present is explicit, rather than the
 *     pipeline quietly producing silence.
 *
 * libopus is stateful and 16-bit, and its output is always 48 kHz because Opus
 * runs internally at 48 kHz regardless of the original rate (the reference
 * implementation says the same: "Decode at a fixed 48k",
 * 78/xiaozhi-esp32 main/audio/demuxer/ogg_demuxer.cc:317).  The backend interface
 * therefore takes packets in and gives back PCM at the decoder's rate, and the
 * caller resamples.
 *
 * PARAMETERS FROM THE PROTOCOL
 *
 *   16000 Hz in, mono, 60 ms frames, per the protocol document and
 *   OPUS_FRAME_DURATION_MS in 78/xiaozhi-esp32 main/audio/audio_service.h:40.
 */

#ifndef VOX_OPUS_DEC_H
#define VOX_OPUS_DEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/* Opus always decodes at 48 kHz regardless of the rate in the stream header --
 * the reference says so explicitly: "Decode at a fixed 48k",
 * 78/xiaozhi-esp32 main/audio/demuxer/ogg_demuxer.cc:315-317.  The 16 kHz in the
 * protocol document is the rate the audio was captured at.
 *
 * Defined here rather than taken from vox_ogg_opus.h: this is a property of the
 * codec, not of the framing, and the decoder must not depend on the demuxer's
 * header to know how many samples a packet produces. */
#define VOX_OPUS_DECODE_RATE 48000
#define VOX_OPUS_DECODE_CHANNELS 1

/* Return codes.  They are separate because they mean different things to a
 * caller: VOX_OPUS_ERR_PACKET is a bad or lost packet and the stream continues,
 * while VOX_OPUS_ERR_TOO_SMALL is the caller's buffer being too short, which is
 * a sizing bug that will recur on every packet until it is fixed.  Counting them
 * together hides that. */
#define VOX_OPUS_ERR_BAD_ARG   (-1)
#define VOX_OPUS_ERR_TOO_SMALL (-2) /* output buffer too short for this packet */
#define VOX_OPUS_ERR_PACKET    (-3) /* malformed or undecodable packet */

/* PCM format the decoder produces: signed 16-bit, interleaved. */
typedef struct
{
  int16_t *data;
  size_t   cap_frames;  /* capacity in samples, i.e. per-channel frames */
} vox_pcm_buf_t;

/* A decoder backend.  libopus implements this. */
typedef struct vox_opus_backend vox_opus_backend_t;

typedef struct vox_opus_backend_ops
{
  void *(*create)(int channels, int rate);
  void  (*destroy)(void *state);
  /* Decodes one packet into pcm; returns frames written, or a negative value. */
  int   (*decode)(void *state, const uint8_t *pkt, size_t len, int16_t *out,
                  int max_frames);
  /* How many PCM frames a packet of `len` bytes would produce, for sizing. */
  int   (*frames_per_packet)(void *state, const uint8_t *pkt, size_t len);
} vox_opus_backend_ops_t;

typedef struct
{
  int      channels;
  int      rate;          /* decoder output rate, 48000 for libopus */
  void    *state;
  const vox_opus_backend_ops_t *ops;
  uint32_t packets;
  uint32_t errors;
  uint32_t samples;       /* total PCM frames produced */
  uint32_t since_report;
} vox_opus_dec_t;

/* Installs a backend.  Returns false if either pointer is NULL.  Called once,
 * before vox_opus_dec_init(). */
bool vox_opus_set_backend(const vox_opus_backend_ops_t *ops, void *factory_ctx);

/* True when a backend is installed.  Used by callers that must degrade
 * deliberately rather than silently. */
bool vox_opus_backend_present(void);

bool vox_opus_dec_init(vox_opus_dec_t *d);
void vox_opus_dec_reset(vox_opus_dec_t *d);
void vox_opus_dec_free(vox_opus_dec_t *d);

/* Decodes one Opus packet into out, which must hold max_frames samples per
 * channel.  Returns frames written (0 is legitimate: a packet may produce no
 * output), or negative on error. */
int vox_opus_decode(vox_opus_dec_t *d, const uint8_t *pkt, size_t len,
                    int16_t *out, int max_frames);

/* Introspection, for logs. */
const char *vox_opus_backend_name(void);

#ifdef __cplusplus
}
#endif

#endif /* VOX_OPUS_DEC_H */