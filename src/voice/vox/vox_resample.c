/*
 * vox_resample.c -- 48 kHz to 16 kHz by linear interpolation.
 * See vox_resample.h for why this step exists and why it carries state.
 */

#include "vox_resample.h"

#include <string.h>

void vox_resample_init(vox_resample_t *rs)
{
  if (rs != NULL)
    {
      memset(rs, 0, sizeof(*rs));
      /* Pre-loaded so the first input sample immediately yields an output;
       * see the note in vox_resample_48k_to_16k(). */
      rs->acc = VOX_RS_RATIO_NUM - 1;
    }
}

void vox_resample_reset(vox_resample_t *rs)
{
  if (rs != NULL)
    {
      rs->acc    = VOX_RS_RATIO_NUM - 1;
      rs->prev   = 0;
      rs->primed = false;
    }
}

size_t vox_resample_out_frames(size_t n_in)
{
  return n_in / VOX_RS_RATIO_NUM;
}

size_t vox_resample_48k_to_16k(vox_resample_t *rs, const int16_t *in,
                               size_t n_in, int16_t *out, size_t out_cap)
{
  size_t o = 0;
  size_t i;

  if (rs == NULL || in == NULL || out == NULL || n_in == 0)
    {
      return 0;
    }

  for (i = 0; i < n_in; i++)
    {
      int32_t cur = in[i];

      /* `acc` counts input samples since the last output.  Reaching
       * RATIO_NUM means one output sample is due, positioned between rs->prev
       * and cur.
       *
       * The very first output has no predecessor, so before the loop starts
       * acc is pre-loaded to RATIO_NUM-1: the first input sample then produces
       * an output immediately (using itself as both endpoints), and every
       * subsequent one lands on the 3-sample spacing.  Starting from 0 instead
       * costs the first output and yields 959 samples for a 2880-sample frame,
       * which is the off-by-one this comment exists to prevent. */
      while (rs->acc >= VOX_RS_RATIO_NUM)
        {
          int32_t frac;
          int32_t v;

          if (o >= out_cap)
            {
              /* The buffer is too small.  Leave acc alone and report what was
               * produced: dropping samples here would shift every later one,
               * which is worse than a short read. */
              rs->in_frames += i;
              return o;
            }

          rs->acc -= VOX_RS_RATIO_NUM;
          frac     = rs->acc;

          if (!rs->primed)
            {
              out[o++] = (int16_t)cur;
            }
          else
            {
              v = rs->prev + ((cur - rs->prev) * frac) / VOX_RS_RATIO_NUM;
              out[o++] = (int16_t)v;
            }

          rs->prev = cur;
        }

      rs->acc++;
    }

  rs->in_frames  += n_in;
  rs->out_frames += (uint32_t)o;
  return o;
}
