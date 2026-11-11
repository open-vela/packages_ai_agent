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

/*
 * CP-side (路 B) no-op stubs for the ai_agent UI renderer.
 *
 * lvgl_ui_channel.c is shared with the AP build and calls into the voice
 * channel (TTS/ASR) and the agent message bus.  On the CP core we only
 * RENDER — TTS/ASR live on AP, and the PTT → inbound-bus path is not wired
 * yet (M5 will forward PTT events back to AP over the UI RPMsg endpoint).
 * These no-op definitions let the UI subset link on CP without dragging
 * the voice/core/llm/TLS subsystems into the CP image.
 *
 * They are compiled ONLY into the EXAMPLES_AI_AGENT_UI_CP app; the AP
 * EXAMPLES_AI_AGENT_VELA app links the real implementations, so there is
 * no symbol clash.
 */

#include <stddef.h>

#include "voice/voice_channel.h"
#include "core/message_bus.h"
#include "agent_compat.h"

/* The CP renderer has no config store of its own (config_store.c is not part
 * of this app).  Report every key as absent and swallow writes: pet_status_view
 * then picks a species at random for this boot instead of persisting one. */
int claw_config_get(const char *key, char *buf, size_t buf_size)
{
  (void)key;
  if (buf != NULL && buf_size > 0)
    {
      buf[0] = '\0';
    }
  return ERROR;
}

int claw_config_set(const char *key, const char *value)
{
  (void)key;
  (void)value;
  return ERROR;
}

int voice_channel_init(void)                                 { return 0; }
int voice_channel_start(void)                                { return 0; }
int voice_channel_stop(void)                                 { return 0; }

int voice_channel_stop_with_text(char *text_out, size_t text_cap)
{
  /* CP does not run ASR; return an empty transcription so the PTT callback
   * (which is itself stubbed out of the active flow for now) treats this
   * as "no speech" rather than erroring. */
  if (text_out && text_cap > 0)
    {
      text_out[0] = '\0';
    }
  return 0;
}

/* CP only renders; an utterance produced here has nowhere to go locally.
 * M5 will forward PTT events back to AP over the UI RPMsg endpoint. */
void voice_channel_dispatch(const char *text)                { (void)text; }

int voice_channel_speak(const char *text)                    { (void)text; return 0; }
int voice_channel_beep(unsigned int freq_hz, unsigned int ms) { (void)freq_hz; (void)ms; return 0; }
int voice_channel_test_tts(const char *text, const char *out) { (void)text; (void)out; return 0; }
int voice_channel_test_asr(const char *pcm_path)             { (void)pcm_path; return 0; }

int message_bus_push_inbound(const agent_msg_t *msg)         { (void)msg; return 0; }
