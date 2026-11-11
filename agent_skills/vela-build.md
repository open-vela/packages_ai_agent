# Vela Build

Compile vela firmware on the PC via the `pc` MCP server, and report progress and result.

## When to use
When user asks to:
- 编译固件 / 编译 vela / build the firmware
- 查看编译进度或结果（编译好了吗）
- clean 全量重编

## How to use

### 1. Ensure the pc server is connected
- `mcp_status` — check a server named `pc` is present and connected
- If missing (first use only, config is persisted):
  - `run_shell "mcp_add pc http://10.0.2.2:8760/mcp"`
  - `run_shell "mcp_discover"`

### 2. Start the build (async!)
- `pc.vela_build {"target": "goldfish-arm64-v8a-ap", "clean": false}`
- target defaults to `goldfish-arm64-v8a-ap`; `clean: true` = full rebuild (much slower)
- Returns `started` immediately — do NOT wait for the build in this call.
  The build runs on the PC; a single HTTP request would time out at 120 s.

### 3. Poll status (with server-side wait)
- `pc.vela_build_status {"wait": 60}`
- `wait` holds the call up to 60 s while the build runs, then returns the
  current state — repeat until terminal. Never poll in a tight loop: the
  running-state reply is small, but each round trip costs an LLM turn.
- `state: running` → call `vela_build_status {"wait": 60}` again
- `state: success` → report artifact (e.g. vela_ap.bin + size) and elapsed time
- `state: failed` → report the error lines from `log_tail`

The PC keeps the full log; `log_tail` (last ~2.5 KB) is only included on
failure — the running-state reply is a one-liner by design.

## Limits
- Only one build at a time on the PC (a second `vela_build` while running is rejected)
- Build output stays on the PC under `out/openvela_vela_<target>/`

## Example
User: "帮我编译一下固件"
→ mcp_status
→ pc.vela_build {"target": "goldfish-arm64-v8a-ap"}
→ pc.vela_build_status {"wait": 60} (repeat until terminal)
→ "编译成功：vela_ap.bin 12.3 MB，耗时 35 秒"

User: "编译失败了吗"
→ pc.vela_build_status {}
→ report state; if failed, read the error lines in log_tail
