# 月薪喵主动恢复教练

使用确定性端侧策略评估来源明确的手环状态，并通过持久化 cron 任务主动执行恢复提醒。首个里程碑只接受明确标注为 simulated 的演示数据，不能描述为真实传感器测量。

## When to use

当用户要求查询恢复建议、开启主动恢复教练、设置久坐提醒，或检查教练任务时使用。

## How to use

1. 先调用 `get_current_time` 获取当前时间；创建一次性任务时必须据此计算 `at_epoch`。
2. 明确确认数据来源。当前里程碑只能把 `source=simulated` 传给 `mooncat_coach_tick`；不得把 UI 生成的心率、压力、睡眠或活动值写成实机数据。
3. 只查询建议时调用 `mooncat_coach_tick`，设置 `mode=preview`，并完整传入 `valid`、`workout_active`、`do_not_disturb`、`inactivity_minutes`、`sleep_debt_minutes`、`stress_score` 和 `battery_percent`。
4. 需要立即演示执行时，把同一观察传给 `mooncat_coach_tick`，设置 `mode=execute`。只有策略命中且不在 cooldown 时，工具才会向 `mooncat` channel 推送带 `[DEMO]` 的通知。
5. 需要主动任务时，先调用 `cron_list` 去重，再调用 `cron_add`。`schedule_type` 只能是 `every` 或 `at`；分别使用 `interval_s` 或 `at_epoch`。设置 `channel=system` 作为 cron 框架审计回执，并设置 `action=mooncat_coach_tick`；动态建议由 Tool 自己推送到 `mooncat` channel，避免 UI 双重弹窗。`action_args` 必须是字符串化 JSON，不能传 JSON 对象。
6. 解释工具返回的 `status`：`delivered` 表示已执行消息总线推送，`preview` 表示只返回建议，`noop` 表示无建议或仍在 cooldown。始终向用户显示 `evidence_boundary=demo_only_not_physical_sensor`。
7. LLM、消息总线或工具失败时明确报告失败；不得声称任务或提醒已成功执行。

## Cron example

```json
{
  "name": "mooncat-demo-check",
  "schedule_type": "every",
  "interval_s": 2700,
  "message": "[DEMO] MoonCat scheduled policy check",
  "channel": "system",
  "chat_id": "mooncat-coach",
  "action": "mooncat_coach_tick",
  "action_args": "{\"mode\":\"execute\",\"source\":\"simulated\",\"valid\":true,\"workout_active\":false,\"do_not_disturb\":false,\"inactivity_minutes\":45,\"sleep_debt_minutes\":0,\"stress_score\":20,\"battery_percent\":80}"
}
```

## Example

用户：“看看月薪喵现在建议我做什么。”

→ `mooncat_coach_tick` with `mode=preview`, `source=simulated`

→ “演示策略建议伸展 60 秒。该结果来自 simulated 数据，不是 Gemini S1 传感器实测。”

## Scheduler constraints

- 官方 `cron_service` 每 10 秒检查一次到期任务，所以触发存在最多约一个检查周期的延迟，并非硬实时秒级。
- cron 在持有内部 mutex 时调用 action；`mooncat_coach_tick` 及其依赖不得调用 `cron_add`、`cron_list` 或 `cron_remove`，否则可能自锁。
- cron 会在 action 之后无条件发送 job 的 `message`。示例把框架回执放在 `system` channel；只有 Tool 的动态建议进入 `mooncat` UI channel。interval 设为 2700 秒，message 保留 `[DEMO]`，避免把内部检查冒充真实健康事件。
