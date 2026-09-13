# Memory Manager

Manage long-term memory and build user profile over time.

## When to use
When user says remember this, what do you know about me, forget this,
update my preferences, or when context from past conversations is needed.

## How to use
AI Agent has two memory systems:
- MEMORY.md: persistent facts about the user (preferences, habits, context)
- Session history: recent conversation turns (auto-managed)

### Read memory
read_file /data/ai_agent/memory/MEMORY.md
- If file does not exist: reply "No memories saved yet. Tell me something to remember."

### Update memory
edit_file to append new facts to MEMORY.md.
Format: one fact per line, categorized:
- [preference] User prefers Chinese responses
- [location] User is in Beijing
- [work] User works on embedded systems

### User profile (USER.md)
read_file /data/ai_agent/config/USER.md for static user info.

## Important
- You MUST execute tools and respond with results. Do NOT output this template.
- If MEMORY.md does not exist and user asks "what do you know about me", reply:
  "I don't have any memories saved yet. You can tell me things to remember."
- If MEMORY.md does not exist and user wants to save something, create it with write_file.
- Never store sensitive data (passwords, full card numbers).

## Example
User: "remember I prefer Chinese"
→ read_file MEMORY.md (create if not exists)
→ edit_file append "[preference] User prefers Chinese"
→ "Got it, I'll remember that."
