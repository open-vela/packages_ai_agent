# Task Manager

Manage a TODO list with add, complete, and view.

## When to use
When user says add task, TODO, done with X, what's pending.

## How to use
Task file: /data/ai_agent/TASKS.md
- View: read_file the task file
- Add: get_current_time, then edit_file/write_file to append: - [ ] [YYYY-MM-DD] desc
- Complete: edit_file to change - [ ] to - [x]

## Example
User: "添加待办：完成代码审查"
→ get_current_time → 2026-03-28
→ write_file /data/ai_agent/TASKS.md append "- [ ] [2026-03-28] 完成代码审查"
→ "已添加待办：完成代码审查"

User: "看看我的待办"
→ read_file /data/ai_agent/TASKS.md
→ "你的待办：
   - [ ] [2026-03-28] 完成代码审查
   - [x] [2026-03-27] 修复 TLS bug"
