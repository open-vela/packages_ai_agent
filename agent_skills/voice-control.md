# Voice Control

Manage voice channel settings and test audio hardware.

## When to use
When user says start listening, stop listening, test microphone,
test speaker, voice settings, or audio check.

## How to use
Voice channel is managed via CLI commands (run_shell):
- Start voice: run_shell "voice_start"
- Stop voice: run_shell "voice_stop"
- Test TTS: run_shell "voice_test_tts <text>"
- Test ASR: run_shell "voice_test_asr <pcm_file>"
- Check TTS backend: run_shell "set_voice_tts"
- Check ASR backend: run_shell "set_voice_asr"

## Diagnostics
If voice isn't working:
1. run_shell "voice_test_tts hello" — test TTS output
2. Check if volc credentials are set: read_file /data/ai_agent/config/config.json
3. Look for volc_appkey, volc_token, volc_api_key entries
4. If missing: tell user to run set_volc_key and set_volc_asr commands

## Example
User: "开始语音监听"
→ run_shell "voice_start"
→ "语音通道已启动，正在监听。"

User: "测试一下扬声器"
→ run_shell "voice_test_tts 你好"
→ "TTS 测试完成，你应该听到了'你好'。"
