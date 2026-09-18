# AI Coding 闭环运行日志 — Gerrit Change 10334000

本文件记录 openvela on-device `ai_agent` 通过 **MCP 工具链**驱动的"读需求 → 写码 →
编译验证 → 提交 → 推送 Gerrit"端到端闭环的真实运行记录。所有推送都发生在真实
内网 Gerrit：<https://gerrit.pt.mioffice.cn/c/vela/apps/+/10334000>

## 架构

```
QEMU 内 ai_agent (ReAct loop)
      │  MCP over HTTP (10.0.2.2:8760)
      ▼
remote_ctrl MCP server (PC 侧, 工作区 = apps 仓 worktree)
  tools: rc_read_file / rc_write_patch / rc_run / rc_git / rc_repo_open / rc_summary
      │  rc_run 执行 gcc / git commit --amend / git push
      ▼
真实 Gerrit: ssh://gerrit.pt.mioffice.cn:29418/vela/apps  (refs/for/trunk-25rc05)
```

- 任务经工作区文件 `AGENT_TASK.txt` 下发（读 → 实现 → 编译 → amend 提交 → push → 查 PS → 播报）。
- 每次提交用 `git commit -a --amend --no-edit` 保留同一 Change-Id，因此推送体现为同一
  Change 的新 patchset（`updated`，而非 `new`）。
- patchset 编号由 `python3 /tmp/gerrit_ps.py` 向 Gerrit 服务器实时查询，agent 播报的是
  服务器权威值（非估算）。

## 目标文件改动（agent 实现的内容）

`apps/examples/hello/hello_main.c`：在基线 TODO 之上新增 `add()` 并在 `main` 打印
`add(3, 4)`：

```c
int add(int a, int b)
{
  return a + b;
}
...
  printf("Hello, World!!\n");
  printf("add(3, 4) = %d\n", add(3, 4));
```

## 工具调用记录（remote_ctrl 服务器端日志，节选多轮）

每一轮 agent 自主执行的动作序列一致：

```
TOOL_CALL rc_read_file   {"path":"AGENT_TASK.txt"}
TOOL_DONE  -> 任务：为 examples/hello/hello_main.c 实现 add 函数 ...
TOOL_CALL rc_write_patch {"path":"examples/hello/hello_main.c", ...}
TOOL_DONE  -> wrote 1803 bytes to examples/hello/hello_main.c (mode=overwrite)
TOOL_CALL rc_run {"cmd":"gcc -I/tmp/hello_stub examples/hello/hello_main.c -o /tmp/hello_out"}
TOOL_DONE  -> rc=0
TOOL_CALL rc_run {"cmd":"git commit -a --amend --no-edit"}
TOOL_DONE  -> rc=0  [detached HEAD <sha>] examples/hello: ... 1 file changed, 9 insertions(+), 1 deletion(-)
TOOL_CALL rc_run {"cmd":"git push vela HEAD:refs/for/trunk-25rc05"}
TOOL_DONE  -> rc=0  remote: Processing changes: updated: 1 ... SUCCESS
TOOL_CALL rc_run {"cmd":"python3 /tmp/gerrit_ps.py"}
TOOL_DONE  -> rc=0  GERRIT_CURRENT_PATCHSET=<n> GERRIT_REVISION=<rev> GERRIT_URL=...
```

## Patchset 进度（Gerrit 服务器回读，权威）

| Patchset | commit / revision | 推送结果 | 说明 |
|----------|-------------------|----------|------|
| PS1 | 9075dd236 (baseline) | new | 人工基线（TODO 占位） |
| PS2 | adbd25a9a | updated | agent 首次实现闭环 |
| PS5 | bafdee3988a5 | updated | agent 全自动闭环 |
| PS6 | c5442365ca6e | updated | 加入真实 PS 号查询 + 提示音 |
| PS7 | fa64b10b1cc8 | updated | 完整重跑（全新 boot → setup → 闭环） |

> 每个 patchset 均由 `ssh gerrit query --current-patch-set change:10334000` 独立回读确认，
> agent 气泡播报的 Patchset 号与服务器 `currentPatchSet` 完全一致。

## Agent 最终播报（LVGL 气泡，示例：PS7）

```
🎉 任务完成 — Gerrit 评审报告
| 步骤 | 状态 | 结果 |
| 写文件 | ✅ | hello_main.c 覆盖写入 1803 bytes，含 add() 函数 |
| 编译验证 | ✅ | gcc 编译通过，rc=0 |
| Commit amend | ✅ | commit fa64b10b1，1 file changed, 9 insertions(+), 1 deletion(-) |
| Push Gerrit | ✅ | SUCCESS，已推送到 refs/for/trunk-25rc05 |
Gerrit: Change 10334000 · Patchset 7 · Revision fa64b10b1cc8
URL: https://gerrit.pt.mioffice.cn/c/vela/apps/+/10334000
```

完成后设备本地合成提示音（`voice_test_beep 1500Hz 500ms` → `Beep done: 0`），OBS 收音。

## 相关配置改动

- `include/agent_config.h`：`AGENT_LLM_TIMEOUT_SEC` 60→120、`AGENT_LLM_SOCKET_TIMEOUT_SEC`
  120→180 —— 使多轮工具调用后的"最终总结" LLM 请求不再被看门狗判超时，气泡完整播报。

<!-- CI re-trigger: aurix runner lost communication (infra flake), 4/5 boards green; re-running. -->
