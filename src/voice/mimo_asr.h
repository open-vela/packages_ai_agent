/*
 * mimo_asr.h -- public surface of the Xiaomi MiMo cloud ASR backend.
 *
 * The backend registers itself with the voice_asr framework under the name
 * "mimo".
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "voice/voice_asr.h"

#define MIMO_ASR_BACKEND_NAME "mimo"

/* Register the backend (also makes it the selected backend). */
int mimo_asr_register(void);

/* Recognize PCM (16 kHz / 16-bit / mono) and write the transcript into
 * text_out (NUL-terminated, capped at text_cap). Returns 0 on success.
 * Declared here so the voice_asr framework can hold a static reference that
 * survives --gc-sections / LTO. */
int mimo_asr_recognize(const unsigned char *pcm_data,
                       size_t pcm_len,
                       char *text_out,
                       size_t text_cap);

#ifdef __cplusplus
}
#endif
