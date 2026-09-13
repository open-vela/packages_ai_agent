# Feishu Integration Test

Test Feishu Bot capabilities end-to-end.

## When to use
When user says test feishu, feishu test, or verify feishu connection.

## How to use
Run these tests in sequence, report each result:
1. get_current_time - verify time
2. get_weather location=Beijing - verify weather
3. write_file + read_file a test file - verify file I/O
4. read_file /data/ai_agent/memory/MEMORY.md - verify memory
5. cron_list - verify cron
6. Summarize all results with pass/fail status

## Example
User: "测试飞书连接"
→ get_current_time ✅
→ get_weather ✅
→ write_file + read_file ✅
→ read_file MEMORY.md ✅
→ cron_list ✅
→ "飞书集成测试结果：5/5 通过，所有功能正常。"
