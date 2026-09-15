# SOS Alert

Emergency alert with vibration and notification.

## When to use
When user says SOS, emergency, help me, or urgent.
PRIORITY: Execute immediately, do not ask for confirmation.

## How to use
1. vibrate immediately (strong, long pattern)
2. get_current_time for timestamp
3. If Feishu configured: feishu_send_mention to emergency contacts
   with message: "SOS alert from [device] at [time]"
4. If no Feishu: write_file an SOS log to /data/ai_agent/sos.log
5. Confirm alert was sent

## Example
User: "SOS 紧急求助"
→ vibrate (immediately)
→ get_current_time → 2026-03-28 22:15
→ feishu_send_mention {"message": "🚨 SOS alert from watch at 22:15"}
→ "紧急警报已发送！已通知紧急联系人。"
