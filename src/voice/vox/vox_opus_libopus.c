/*
 * vox_opus_libopus.c -- the libopus implementation of the decode backend.
 *
 * This is the only file that includes <opus.h>.  libopus is cross-compiled for
 * this target out of tree (backend/README-opus.md records the exact configure
 * line), so the firmware build links a prebuilt libopus.a rather than compiling
 * the library inside the NuttX tree.
 *
 * WHY THE ERROR CHECKS ARE WHAT THEY ARE
 *
 * opus_decode() returns negative on a malformed packet.  The pipeline must not
 * die on one bad packet -- a single corrupted frame on a lossy link is normal --
 * but it must also not pretend the packet decoded.  So a negative return is
 * counted and passed up, and the decoder state is left alone: libopus is
 * documented to leave the decoder usable after opus_decode() fails, so calling
 * again on the next packet is correct rather than hopeful.
 *
 * OpusDecodeFunc() is requested rather than opus_decode() because it exposes the
 * packet's own frame count, which lets the caller size the buffer for a long
 * packet (a 120 ms packet is 5760 samples per channel) without hard-coding a
 * maximum.  VOICE_CHANNEL_ROUTING and CONFIGURATION are left to the library,
 * which is what a Mono stream wants.
 */

#include "vox_opus_dec.h"

#include <opus.h>
#include <stdlib.h>
#include <string.h>

#include <syslog.h>
#include <nuttx/syslog/syslog.h>

/* [V890] Replacement for the libopus celt_fatal() that
 * external/opus/opus/celt/arch.h now omits (OVERRIDE_celt_fatal).
 *
 * The stock version is
 *   fprintf(stderr, "Fatal (internal) error in %s, line %d: %s\n", ...);
 *   abort();
 * Under NuttX stderr is NULL, so that fprintf faults on a null FILE
 * (LoadProhibited, vaddr 0x0000000c) before a single character reaches the
 * console, and abort() has nothing useful to do either.  That crash is what
 * [V646] UP pc=... -> celt_fatal actually was: not an Opus decoding bug,
 * just the error reporting path itself exploding.
 *
 * Here the failing assertion - its text, file and line - is written with
 * syslog() so it really lands on the serial console, then the CPU is parked
 * so the message is not scrolled away by whatever runs next.  The loop also
 * satisfies the __attribute__((noreturn)) on the declaration in arch.h.
 *
 * If this ever fires, str/file/line identify the exact celt_assert() that
 * failed inside libopus, which is the actual bug to chase.
 */

#ifdef __GNUC__
__attribute__((noreturn))
#endif
void celt_fatal(const char *str, const char *file, int line)
{
  syslog(LOG_ERR, "[V890] OPUS FATAL: %s @ %s:%d\n",
         str ? str : "(null)", file ? file : "(null)", line);
  syslog_flush();

  for (;;)
    {
      ;
    }
}


typedef struct
{
  OpusDecoder *dec;
  int          channels;
  int          rate;
} libopus_state_t;

static void *lb_create(int channels, int rate)
{
  libopus_state_t *s;
  int              err;

  s = (libopus_state_t *)calloc(1, sizeof(*s));

  if (s == NULL)
    {
      return NULL;
    }

  s->channels = channels;
  s->rate     = rate;
  s->dec = opus_decoder_create(rate, channels, &err);

  if (s->dec == NULL || err != OPUS_OK)
    {
      free(s);
      return NULL;
    }

  return s;
}

static void lb_destroy(void *state)
{
  libopus_state_t *s = (libopus_state_t *)state;

  if (s == NULL)
    {
      return;
    }

  if (s->dec != NULL)
    {
      opus_decoder_destroy(s->dec);
    }

  free(s);
}

static int lb_decode(void *state, const uint8_t *pkt, size_t len, int16_t *out,
                     int max_frames)
{
  libopus_state_t *s = (libopus_state_t *)state;
  int              n;

  if (s == NULL || s->dec == NULL)
    {
      return -1;
    }

  /* opus_decode() takes an int length; a packet larger than that cannot come
   * from a 60 ms frame and would be a framing bug rather than a valid packet. */
  if (len > (size_t)0x7FFFFFFF)
    {
      return -1;
    }

  n = opus_decode(s->dec, pkt, (opus_int32)len, out, max_frames, 0);

  /* The two failure kinds are kept apart so the caller can tell a bad packet
   * (recoverable, the link simply lost one) from its own buffer being too
   * short (a sizing bug that will repeat on every packet). */
  if (n == OPUS_BUFFER_TOO_SMALL)
    {
      return VOX_OPUS_ERR_TOO_SMALL;
    }

  if (n < 0)
    {
      return VOX_OPUS_ERR_PACKET;
    }

  return n;
}

static int lb_frames_per_packet(void *state, const uint8_t *pkt, size_t len)
{
  libopus_state_t *s = (libopus_state_t *)state;

  if (s == NULL || s->dec == NULL || pkt == NULL)
    {
      return 0;
    }

  if (len > (size_t)0x7FFFFFFF)
    {
      return 0;
    }

  return opus_decoder_get_nb_samples(s->dec, pkt, (opus_int32)len);
}

static const vox_opus_backend_ops_t LB_OPS = {
  lb_create, lb_destroy, lb_decode, lb_frames_per_packet
};

/* Called once at startup.  Returns false when the library is missing, which the
 * caller should treat as "this build has no audio output" rather than as an
 * error to retry. */
bool vox_opus_libopus_install(void);
bool vox_opus_libopus_install(void)
{
  return vox_opus_set_backend(&LB_OPS, NULL);
}