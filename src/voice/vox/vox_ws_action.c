#include "vox_ws_action.h"

vox_action_e vox_ws_action_for(const vox_msg_t *msg)
{
  if (msg == NULL)
    {
      return VOX_ACT_ERROR;
    }

  switch (msg->event)
    {
      /* The session is driving itself here; the application only needs to know
       * it may start sending audio, which the transport already knows. */
      case VOX_EV_SESSION_READY:
      case VOX_EV_CONNECTED:
      case VOX_EV_TTS_SENTENCE_END:
        return VOX_ACT_NONE;

      case VOX_EV_STT_TEXT:
        return VOX_ACT_SHOW_USER_TEXT;

      /* The one that matters: a finished sentence is speakable right now,
       * while the model is still producing the next one. */
      case VOX_EV_TTS_SENTENCE:
        return VOX_ACT_SPEAK_SENTENCE;

      case VOX_EV_TTS_START:
        return VOX_ACT_NONE;

      case VOX_EV_TTS_STOP:
        return VOX_ACT_PLAYBACK_DONE;

      case VOX_EV_EMOTION:
        return VOX_ACT_SHOW_EMOTION;

      case VOX_EV_MCP_CALL:
        return VOX_ACT_MCP_CALL;

      case VOX_EV_SESSION_ENDED:
      case VOX_EV_DISCONNECTED:
        return VOX_ACT_ENDED;

      case VOX_EV_AUDIO_DATA:
        /* Opus is not decoded yet.  Reporting NONE would hide that the audio
         * went nowhere, so it is surfaced as an error rather than dropped. */
        return VOX_ACT_ERROR;

      default:
        return VOX_ACT_ERROR;
    }
}


const char *vox_ws_action_name(vox_action_e a)
{
  switch (a)
    {
      case VOX_ACT_NONE:           return "none";
      case VOX_ACT_SPEAK_SENTENCE: return "speak";
      case VOX_ACT_SHOW_USER_TEXT: return "user_text";
      case VOX_ACT_SHOW_EMOTION:  return "emotion";
      case VOX_ACT_MCP_CALL:       return "mcp";
      case VOX_ACT_PLAYBACK_DONE: return "playback_done";
      case VOX_ACT_ENDED:         return "ended";
      case VOX_ACT_ERROR:          return "error";
      default:                     return "?";
    }
}
