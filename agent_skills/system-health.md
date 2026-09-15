# System Health Check

Check AI Agent system status and summarize key info.

## When to use
When user asks about system status, health check, or running state.

## How to use
1. get_current_time to get current time
2. list_dir to list /data/ai_agent/ files
3. read_file /data/ai_agent/config/config.json to check config
4. cron_list to check scheduled tasks
5. Summarize: time, file count, config status, cron jobs

## Example
User: "检查系统状态"
→ get_current_time → 2026-03-28 10:00
→ run_shell "free" → total 512KB, used 320KB
→ read_file /proc/uptime → 3600s
→ "系统运行正常：内存 320/512KB (62%)，运行时间 1 小时。"
