# Voice Memo

Capture voice input as text notes.

## When to use
When user says record this, voice memo, save what I said,
or dictate a note — especially from the voice channel.

## How to use
1. The voice channel has already converted speech to text
2. get_current_time for timestamp
3. Path: /data/ai_agent/memory/daily/YYYY-MM-DD.md
4. read_file to check if today's diary exists
5. If exists: edit_file to append. If not: write_file to create
6. Format: - [HH:MM] 🎤 <transcribed content>
7. Confirm briefly: "Saved your voice memo."

## Note
The 🎤 emoji prefix distinguishes voice memos from typed notes.

## Example
User (voice): "记一下，明天下午三点开会"
→ get_current_time → 2026-03-28 14:20
→ read_file /data/ai_agent/memory/daily/2026-03-28.md (exists)
→ edit_file append "- [14:20] 🎤 明天下午三点开会"
→ "已保存语音备忘。"
