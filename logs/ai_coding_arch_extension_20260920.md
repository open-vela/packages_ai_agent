# AI Coding 开发日志 — 架构扩展：PC 侧编译 MCP + media 播放（2026-09-20）

本文件记录 2026-09-20 一轮 AI 主导开发（Claude Code，方向由团队对话确定）的
真实过程与运行证据：设备端 `ai_agent` 新增「PC 侧编译」腿、远程编码完成播报、
播放切换 media 框架优先，以及 BK7258 板上 media 框架可行性判定（no-go）。

## 架构（改动后）

```
QEMU 内 ai_agent (ReAct loop,「编译 vela」builtin skill)
      │  MCP over HTTP (10.0.2.2:8760)
      ▼
remote_ctrl MCP server (PC 侧, 本轮入仓 pc/)
  tools: rc_repo_open/rc_read_file/rc_write_patch/rc_run/rc_git/rc_summary
       + vela_build / vela_build_status (新增)
      │  vela_build 起后台线程: unset CROSSDEV; envsetup && lunch && m -j20
      ▼
真实 vela 构建 out/openvela_vela_goldfish-arm64-v8a-ap/vela_ap.bin
```

- `vela_build` 只负责发起（<1 s 返回）；设备侧 HTTP 读超时 120 s
  （`AGENT_LLM_SOCKET_TIMEOUT_SEC`），编译 35 s~数分钟，同步等待必超时。
- `vela_build_status {"wait": 60}`：server 端阻塞最长 90 s（低于设备 120 s 读
  超时）。设备 LLM 无法在轮次间 sleep，`wait` 是它控制轮询节奏的唯一手段；
  一次 wait 间隔取代紧循环轮询。
- running 态回复三行、不带 log_tail；失败才附 log_tail（末 ~2.5 KB 定位错误行）。

## 真实运行记录（server 端日志，2026-09-20 10:53 起）

```
TOOL_CALL vela_build args={"target": "goldfish-arm64-v8a-ap", "clean": false}
TOOL_DONE  -> started: target=goldfish-arm64-v8a-ap clean=False jobs=20
             log=/tmp/vela_build_goldfish-arm64-v8a-ap_20260920-105343.log
TOOL_CALL vela_build_status args={"wait": 60}
TOOL_DONE  -> state: running target: goldfish-arm64-v8a-ap elapsed: 61s
             still building — call again with {"wait": 60}
TOOL_CALL vela_build_status args={"wait": 60}
TOOL_DONE  -> state: success target: goldfish-arm64-v8a-ap elapsed: 69s
             artifact: vela_ap.bin (236.7 MB), nuttx (236.7 MB)
```

当日经 server 触发的三次真实构建（日志均在 `--log-dir`）：

| 时间 | 耗时 | 结果 |
|------|------|------|
| 10:36 | 36 s | build completed successfully |
| 10:40 | 01:08 | build completed successfully |
| 10:53 | 01:09 | build completed successfully（上表轮询即此轮） |

产物核对：`out/openvela_vela_goldfish-arm64-v8a-ap/vela_ap.bin` = 236,712,376 B，
与 server 上报 236.7 MB 一致（本目标 `vela_ap.bin` 为 `nuttx` ELF 拷贝）。

PC 侧单测 `pc/test_mcp.py`：31/31 通过（rc_* 全套、白名单拦截 `rm`、路径逃逸
拒绝、push 拒绝、并发构建拒绝、假构建全流程含 wait 期满/状态变化退出/running
不带 log_tail）。

## qemu e2e（设备端「编译 vela」skill）

设备侧一次 `mcp_add pc http://10.0.2.2:8760/mcp`（config store 持久化）后
`mcp_discover`：gerrit 15 + jira 24 + pc 8 = 47 工具。语音「帮我编译一下固件」
→ skill 引导 `pc.vela_build` → `pc.vela_build_status {"wait": 60}` 轮询至
success → 语音 + LVGL 汇报产物与耗时。配套 `AI_AGENT_MCP_MAX_TOOLS` 48→64
（vendor/openvela 两份 defconfig，未包含在本仓推送内）。

skill 以 builtin 分发（`skill_loader.c` `s_builtins[]` 新增一行注册，
`install_builtin` 仅在 `/data/agent/skills/vela-build.md` 缺失时落盘）——
老固件升级后无需手工拷贝。

## 远程编码完成播报（toast）

远程编码一轮耗时数分钟，此前只有「审批」橙 toast，没有「结束」信号。新增
`REMOTE_EVENT_TURN_TRANSCRIPT` 分支绿 toast（0x4caf50 / 4000 ms，取 transcript
首行截断），复用审批 toast 的 `lvgl_toast_show_async` 原语，+14 行。

无 mimocode 环境下的闭环验证（fake-adapter，见选手仓 661c49c）：

```
bridge 提示 → 橙审批 toast → ronce → permission_result accepted:true
→ fake transcript → 绿完成 toast（截图核对）
```

## 播放切换 media 框架优先

`audio_playback_open` 顺序反转：media_player 优先，`CONFIG_AI_AGENT_AUDIO_NUTTX_DIRECT`
降级为无 media server 板子的回退；media 通道 `MEDIA_STREAM_MUSIC`→`TTS`
（graph.conf 独立 lane `abufsrc@TTS → alsasink@pcm0p`，避开 music 会话冲突）；
`media_player_write_data` 桩签名 int→ssize_t 并处理部分写。

qemu defconfig 移除 direct 后实测：`voice_test_speak` / `voice_test_tts` / beep
出声（direct 已不存在，出声即证明 media 路径）；采集侧 media_recorder 不回归
（`voice_test_asr` 文件式 + 全链路）。

## BK7258 板上 media 框架可行性（spike，结论 no-go）

真实工具链（armv8-m.main / cortex-m33 soft-float）实测：

| 项 | 实测 |
|----|------|
| AP 基线 | `app1.bin` 336,852 B（XIP 2.78 MB，余 2.58 MB；RAM 336 KB） |
| `CONFIG_MEDIA_SERVER=y` | 构建失败：`media_plugin.c:3` 无条件 `#include <libavutil/mem.h>`，全树无 ARMv8-M ffmpeg（prebuilts 仅 A64/x86） |
| 退到 `CONFIG_MEDIA=y`（仅 client） | 仍失败：`utils/media_utils.c:25` `#include <cutils/properties.h>`，AP 无 property 服务 |
| 还原 | defconfig 复原后构建 `decode pass`，镜像与基线逐字节一致 |

结论：media server 本身编不过，不是体积问题；板侧 `pcm0p`/`pcm0c` 维持
NuttX audio 框架，media 框架链路由 qemu 承载。数据详见 `pc/README.md`
「BK7258 板上 media 框架可行性」小节。

## 前置缺陷修复（notify 轮询）

本轮首个提交修复 Gerrit/Jira notify 轮询无法在设备上完成一轮的五个缺陷：

| 缺陷 | 修复 |
|------|------|
| notify 线程 8 KB 栈溢出（TLS 握手递归 assert 杀板） | `AGENT_NOTIFY_STACK` 8→32 KB |
| jira 默认工具名 `jira.search` 与发现名 `jira.jira_search` 不符 | 改用 server 前缀名 |
| 工具/服务器未找到时输出空错误串 | 填 JSON 错误串 |
| Gerrit 网关返回纯文本摘要，仅解析 JSON 致静默为空 | 增加逐行扫描回退 `gerrit_scan_text` |
| 启动期发现输给网络就绪，工具表永久为空 | 每 poll 缺工具时重发现一次 |

实测：gerrit poll 返回 3 条新 change，游标跨 poll/重启去重；jira 穿越网关
flap（失败带原因，上游恢复即自愈）。

## 提交清单（本仓，推送至 cluv/dev-ai-contest-2026）

| 提交 | 内容 |
|------|------|
| 94a77d7 | fix(notify): Gerrit/Jira 轮询端到端修复（五缺陷） |
| 55807e3 | feat(pc): PC 侧 remote_ctrl + vela 编译 MCP server 入仓 |
| 298a254 | feat(skill): builtin「编译 vela」skill + mcp-tools.md 过期 Limits 修正 |
| 52c9ef2 | fix(audio): 播放 media 框架优先，TTS lane 优先 + 部分写处理 |
| 3d64a14 | feat(remote-ctrl): 远程编码轮完成绿 toast |
| 72cbdbe | feat(pc): vela_build_status 服务端 wait 节流 |
| 53a2faa | docs(pc): BK7258 media 框架 spike 结论（no-go） |

配套（其他仓库，不在本次推送范围）：vendor/openvela 双板 defconfig
（media-only 播放 + `MCP_MAX_TOOLS` 64）；选手仓 acp-bridge fake-adapter
transcript（661c49c）。
