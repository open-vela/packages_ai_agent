# Config Wizard

Guide users through AI Agent setup and configuration.

## When to use
When user says setup, configure, settings, how to set up,
or when a feature fails due to missing configuration.

## How to use
1. read_file /data/ai_agent/config/config.json to check current config
2. Identify what's missing based on user's request
3. Guide step by step:

### Essential setup (must-have)
- LLM backend: "run set_llm <preset> <api_key>"
  Recommend: set_llm mimo <key> (Xiaomi, cheapest)
  Or: set_llm deepseek <key> (best value)

### Communication channels (pick one or more)
- Feishu: "run set_feishu_app <app_id> <app_secret>"
- WeChat: "run weixin_login" (scan QR code)
- MQTT: "run set_mqtt <broker_host>:1883"

### Voice (optional)
- TTS: "run set_volc_key <api_key>"
- ASR: "run set_volc_asr <app_id> <token> <cluster>"

### Search (optional)
- Tavily: "run set_tavily_key <key>" (recommended, free tier)
- Or: set_search_key, set_exa_key, set_news_key

### Network (if needed)
- WiFi: "run set_wifi <ssid> <password>"
- Proxy: "run set_proxy <host> <port>"

### Multi-device (optional)
- Gateway: "run set_gateway <host> <port> <token>"

## After setup
run_shell "config_show" to verify all settings.

## Example
User: "帮我设置一下"
→ read_file /data/ai_agent/config/config.json
→ "当前已配置 LLM (kimi)。还缺少：语音(TTS/ASR)、搜索(Tavily)。
   要设置语音，请运行：set_volc_key <你的API密钥>
   要设置搜索，请运行：set_tavily_key <你的密钥>"
