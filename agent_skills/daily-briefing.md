# Daily Briefing

Compile a personalized daily briefing for the user.

## When to use
When the user asks for a daily briefing, morning update, or "what's new today".
Also useful as a heartbeat/cron task.

## How to use
1. Use get_current_time for today's date and time
2. Try to read /data/ai_agent/memory/MEMORY.md for user preferences and context
   - If the file does not exist or is empty, skip this step and use general interests (tech, AI, weather)
3. Try to read today's daily note if it exists
   - If no daily note exists, skip this step
4. Use web_search for relevant news (use user interests from MEMORY.md if available, otherwise search for general tech/AI news)
5. Compile a concise briefing covering:
   - Date and time
   - Weather (if location known from MEMORY.md or USER.md; if unknown, skip or ask)
   - Relevant news/updates based on user interests (or general news if no preferences)
   - Any pending tasks from memory (or "无待办事项" if none)
   - Any scheduled cron jobs (use cron_list; or "无定时任务" if none)

## Important
- You MUST execute the tools above and generate a briefing. Do NOT output this template as your response.
- Even if MEMORY.md does not exist, you should still generate a briefing with at least: current time + general news.
- Always respond in the user's language (detect from their message).

## Format
Keep it brief — 5-10 bullet points max. Use the user's preferred language.

## Example
User: "给我一份今日简报"
→ get_current_time → 2026-03-28 08:00
→ read_file MEMORY.md → (file not found, use default interests)
→ web_search "科技新闻 2026年3月28日"
→ "📋 今日简报 (2026-03-28)
   • 时间：上午 8 点
   • AI：OpenAI 发布新嵌入式推理模型
   • 嵌入式：RISC-V 联盟发布新标准
   • 待办：无未完成任务
   • 定时任务：无"
