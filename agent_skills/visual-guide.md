# Visual Guide

Describe the camera view aloud as an informational aid. This skill does not
guarantee a safe route and must not replace a mobility aid.

## When to use
When the user asks what is ahead, requests a scene description, or asks the
device to read visible text aloud.

## How to use
1. Call `peripheral_status`. In its `devices` array, require the camera and
   `audio_playback` entries to have `available=true`.
2. Call `network_probe` with `apply=false`. A failure is a warning, not a
   reason to skip local hardware checks.
3. Call `camera_capture` with `resolution=high` and ask for observable
   objects, approximate relative positions, and visible text. Never state
   that a route is certainly safe.
4. Extract `analysis` from the result. If it is empty, speak a short failure
   message.
5. Call `tts_speak` with chunks no longer than 100 Chinese characters or
   300 English characters.

## Fallback
- Camera unavailable: use `tts_speak` to report that the camera is not ready.
- TTS unavailable: return the camera analysis as text.
- Network unavailable: do not change DNS automatically; report the
  diagnostic result.
