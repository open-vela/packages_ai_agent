/*
 * vox_opus_dec.c -- packet-to-PCM decoding with an injected backend.
 * See vox_opus_dec.h for why the backend is injected.
 */

#include "vox_opus_dec.h"

#include <string.h>
#include <syslog.h>

static const vox_opus_backend_ops_t *g_ops;

bool vox_opus_set_backend(const vox_opus_backend_ops_t *ops, void *factory_ctx)
{
  (void)factory_ctx;

  if (ops == NULL)
    {
      return false;
    }

  g_ops = ops;
  return true;
}

bool vox_opus_backend_present(void)
{
  return g_ops != NULL;
}

const char *vox_opus_backend_name(void)
{
  return g_ops != NULL ? "installed" : "none";
}

bool vox_opus_dec_init(vox_opus_dec_t *d)
{
  if (d == NULL || g_ops == NULL)
    {
      return false;
    }

  memset(d, 0, sizeof(*d));
  d->ops = g_ops;
  d->state = g_ops->create(1, VOX_OPUS_DECODE_RATE);

  if (d->state == NULL)
    {
      return false;
    }

  d->rate     = VOX_OPUS_DECODE_RATE;
  d->channels = 1;
  return true;
}

void vox_opus_dec_reset(vox_opus_dec_t *d)
{
  if (d == NULL || d->state == NULL || d->ops == NULL)
    {
      return;
    }

  /* The reference resets the decoder between utterances so a lost packet does
   * not leave the CELT state carrying a phantom frame into the next sentence;
   * without it the first packet after a gap can click. */
  d->ops->destroy(d->state);
  d->state = d->ops->create(1, VOX_OPUS_DECODE_RATE);
  d->packets = 0;
  d->errors  = 0;
  d->samples = 0;
}

void vox_opus_dec_free(vox_opus_dec_t *d)
{
  if (d == NULL || d->state == NULL || d->ops == NULL)
    {
      return;
    }

  d->ops->destroy(d->state);
  d->state = NULL;
}

int vox_opus_decode(vox_opus_dec_t *d, const uint8_t *pkt, size_t len,
                    int16_t *out, int max_frames)
{
  int n;

  /* [V890] Report the arguments that make libopus assert.  Only abnormal
   * inputs are logged; a normal call prints nothing.  Caveat: len == 0 is
   * legal Opus (DTX), so it will be reported on every DTX packet - drop it
   * from this test if that floods the console.
   */

  if (pkt == NULL || len == 0 || out == NULL || max_frames <= 0)
    {
      syslog(LOG_ERR,
             "[V890] bad decode args: pkt=%p len=%u out=%p max_frames=%d\n",
             pkt, (unsigned int)len, out, max_frames);
    }


  if (d == NULL || d->state == NULL || d->ops == NULL || out == NULL ||
      max_frames <= 0 || pkt == NULL)
    {
      return VOX_OPUS_ERR_BAD_ARG;
    }

  /* Note the pkt check above: a zero-LENGTH packet is legal Opus (DTX means
   * "nothing changed" but the decoder still advances, and skipping it would
   * desynchronise the stream), while a NULL pointer is a caller bug.  Allowing
   * DTX is why this distinction matters -- relaxing the packet check to accept
   * empty packets must not also admit NULL. */
  n = d->ops->decode(d->state, pkt, len, out, max_frames);

  if (n < 0)
    {
      /* Only a genuinely undecodable packet counts as a stream error.  A short
       * output buffer is the caller's sizing mistake: counting it would inflate
       * the error rate on a perfectly good link and hide the real problem. */
      if (n != VOX_OPUS_ERR_TOO_SMALL)
        {
          d->errors++;
        }

      return n;
    }

  d->packets++;
  d->samples += (uint32_t)n;
  return n;
}