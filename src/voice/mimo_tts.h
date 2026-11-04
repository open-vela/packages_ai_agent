/*
 * mimo_tts.h -- public surface of the Xiaomi MiMo cloud TTS backend.
 *
 * The backend registers itself with the voice_tts framework under the name
 * "mimo" and additionally exposes a streaming entry point so the playback
 * path in voice_channel.c can consume MiMo audio chunk by chunk.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "voice/voice_tts.h" /* voice_tts_chunk_cb */

#define MIMO_TTS_BACKEND_NAME "mimo"

/* Register the backend (also makes it the selected backend). */
int mimo_tts_register(void);

/* Streaming synthesis: PCM chunks are pushed through cb(), the sequence is
 * terminated by cb(NULL, 0, 1, user_data).  Returns 0 or negative errno. */
int mimo_tts_speak_stream(const char *text,
                          voice_tts_chunk_cb cb,
                          void *user_data);

#ifdef __cplusplus
}
#endif
