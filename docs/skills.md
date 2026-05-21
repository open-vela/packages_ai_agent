# Skills 系统

Skills 是 Markdown 格式的可扩展技能，告诉 Agent 在特定场景下如何使用工具完成任务。

## 架构

```
/data/agent/skills/
├── weather.md          ← 内置（首次启动自动安装）
├── daily-briefing.md
├── reminder.md
├── ...                 ← 多个内置 Skill
└── my-custom.md        ← 用户自定义

启动流程：
skill_loader_init()
    │
    ├─ 安装内置 Skill（如不存在）
    │
    ▼
skill_loader_build_summary()
    │
    ├─ 扫描 /data/agent/skills/*.md
    ├─ 提取每个文件的 # 标题 + 描述段落
    │
    ▼
context_builder → 注入系统提示词
    │
    ▼
agent_loop → 根据用户请求匹配 Skill → 按指引调用工具
```

**关键点**：Skill 不是代码，是给 Agent 的"操作手册"。Agent 读取后自主决定调用哪些工具。

## 工作原理

1. Agent 启动时扫描 `/data/agent/skills/` 目录下所有 `.md` 文件
2. 提取每个 Skill 的标题和描述，构建摘要注入系统提示词
3. Agent 根据用户请求自动匹配合适的 Skill 并按其指引执行

## 内置 Skills（示例）

| Skill | 说明 |
|-------|------|
| `weather` | 天气查询和预报 |
| `daily-briefing` | 个性化每日简报 |
| `skill-creator` | 教 Agent 创建新 Skill |
| `reminder` | 定时提醒（基于 cron） |
| `note-taker` | 快速笔记（每日日记） |
| `translate` | 文本翻译 |
| `news-digest` | 新闻摘要 |
| `task-manager` | TODO 任务管理 |
| `system-health` | 系统状态检查 |
| `feishu-test` | 飞书集成测试 |

## 创建自定义 Skill

### 方式一：对话创建

```
vela> ask 帮我创建一个翻译技能
```

### 方式二：手动创建

```bash
adb push my-skill.md /data/agent/skills/
```

### 文件格式

```markdown
# 技能名称

一句话描述。

## When to use
触发条件（关键词越具体越好）。

## How to use
1. 调用 tool_a 获取数据
2. 处理结果
3. 回复用户

## Example
User: "示例输入"
→ web_search "query"
→ "示例回复"
```

### 最佳实践

- 保持简洁（占用 context 窗口）
- 指定具体工具名（`web_search` 而非"搜索"）
- When to use 要精确，避免误触发
- 一个 Skill 一个职责
- 用 Example 降低歧义

### 可用工具

Skill 中可引用 35+ 种内置工具和 MCP 注册的外部工具。完整列表见 [tools.md](tools.md)。
