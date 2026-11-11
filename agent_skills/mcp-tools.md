# MCP Tools

Connect to remote MCP (Model Context Protocol) servers and use their tools alongside built-in tools.

## When to use
When user asks to:
- Connect to an MCP server or add a remote tool service
- List available remote tools
- Use a tool that is not built-in (check remote tools first)
- Manage MCP server connections

## How to use

### Add a server
User provides a name and URL:
- `mcp_add <name> <url>` — add a Streamable HTTP MCP server
- `mcp_add <name> <url> <token>` — add with bearer token auth
- Example: `mcp_add amap https://mcp.example.com/mcp`

⚠️ Security: Use HTTPS URLs in production. HTTP exposes bearer tokens and data to eavesdropping.

### Discover tools
After adding a server, discover its tools:
- `mcp_discover` — sends initialize + tools/list to all servers

### Check available tools
- `mcp_status` — show all servers and connection status
- `mcp_tools` — list all discovered remote tools

### Use remote tools
Remote MCP tools are registered with a `<server>.<tool>` prefix.
For example, adding server "amap" with tool "geocode" registers as `amap.geocode`.
Call them by their full prefixed name just like built-in tools.

### Remove a server
- `mcp_remove <name>` — disconnect and remove a server

## Limits
- Server/tool capacity is set by Kconfig: `AI_AGENT_MCP_MAX_SERVERS` (default 4) and `AI_AGENT_MCP_MAX_TOOLS` (default 16, boards can raise it — goldfish uses 64)
- Servers added with `mcp_add` ARE persisted in the config store and survive reboots; discovered tools refresh at startup/`mcp_discover`

## Troubleshooting
- HTTP 401/403: Check bearer token is valid and not expired
- HTTP 404: Verify URL endpoint is correct
- Connection timeout: Ensure server is reachable (check network/firewall)
- Discovery partial failure: Check `mcp_status` to see which servers connected

## Example
User: "帮我连接高德地图的 MCP 服务"
→ run_shell "mcp_add amap https://mcp.example.com/mcp"
→ run_shell "mcp_discover"
→ "已连接高德地图 MCP 服务，发现 3 个工具：amap.geocode, amap.route_plan, amap.poi_search"

User: "搜索附近的咖啡店"
→ amap.poi_search {"keyword": "咖啡店", "location": "nearby"}
→ 返回搜索结果
