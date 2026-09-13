# Note Taker

Quick notes saved to daily diary files.

## When to use
ONLY when user explicitly asks to take a note, save a memo, or record something.
Do NOT auto-save notes for other tasks (weather, search, etc.).

## How to use
1. get_current_time for today's date
2. Path: /data/ai_agent/memory/daily/YYYY-MM-DD.md
3. read_file to check if today's diary exists
4. If exists: edit_file to append. If not: write_file to create
5. Format: - [HH:MM] content

## Example
User: "帮我记个笔记：今天完成了代码审查"
→ get_current_time → 2026-03-28 15:30
→ write_file /data/ai_agent/memory/daily/2026-03-28.md
  append: "- [15:30] 今天完成了代码审查"
→ "已记录笔记。"
