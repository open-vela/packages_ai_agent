# Board Peripheral Control (LS2K0300)

LOONG-HAT 开发板外设控制技能。通过自然语言控制板载 LED、蜂鸣器、OLED、读取传感器。

对应工具实现：`src/tools/tool_2k0300.c`（驱动逻辑移植自 `apps/examples/ls_driver_test`）。

## When to use

用户提到以下内容时使用：

- **LED 灯**："打开红灯"、"关灯"、"让灯闪 3 下"、"蓝灯调暗"
- **蜂鸣器**："响一声"、"报警"、"beep 三声"
- **按键**："按键现在什么状态"
- **温度**："CPU 多少度"、"板卡温度"
- **ADC 电压**："读一下电位器"、"CH1 电压多少"
- **光照**："现在光照多少 lux"
- **OLED 屏幕**："屏幕上显示 Hello"、"清屏"

## Tools

| 工具 | 功能 | 参数 |
|------|------|------|
| `board_led` | 红(GPIO72)/绿(GPIO73) LED | led: red/green/both; action: on/off/toggle/blink; times, interval_ms |
| `board_pwm` | 蓝 LED PWM 亮度 (GPIO88) | duty: 0-100 (0=停), frequency: 默认 1000Hz |
| `board_buzzer` | 蜂鸣器 (GPIO75, 1kHz) | duration_ms: 1-5000; count: 1-10 |
| `board_key_read` | 读按键 KEY1(87)/KEY2(86) | 无 |
| `board_thermal_read` | CPU 片上温度 | 无 |
| `board_adc_read` | MCP3204 ADC (SPI2) | channel: 0-3，返回 raw + 电压(0-3.3V) |
| `board_light_read` | BH1750 光照 (I2C1) | 无，返回 lux |
| `board_oled_show` | SSD1306 OLED 显示 (I2C1) | text: ASCII ≤256 字符，\n 换行 |

## Examples

### 灯光控制

User: "打开红灯"
→ board_led(led:"red", action:"on")
→ "红灯已打开"

User: "让绿灯闪 5 下，快一点"
→ board_led(led:"green", action:"blink", times:5, interval_ms:200)
→ "绿灯闪烁 5 次，间隔 200ms"

User: "把蓝灯调到 30% 亮度"
→ board_pwm(duty:30)
→ "蓝灯已调至 30% 亮度"

User: "关掉所有灯"
→ board_led(led:"both", action:"off") + board_pwm(duty:0)
→ "所有灯已关闭"

### 声音与传感

User: "报警"
→ board_buzzer(duration_ms:500, count:3)
→ "蜂鸣器报警 3 声"

User: "CPU 多少度"
→ board_thermal_read()
→ "当前 CPU 温度 45.3°C"

User: "现在光照怎么样"
→ board_light_read()
→ "当前光照 235.6 lux"

User: "读一下 ADC"
→ board_adc_read(channel:0)
→ "CH0: raw=2048, 电压 1.65V"

### OLED 显示

User: "屏幕显示 Hello Loong"
→ board_oled_show(text:"Hello Loong")
→ "已显示在 OLED 屏幕上"

User: "显示温度"
→ board_thermal_read() → board_oled_show(text:"Temp: 45.3C")
→ "屏幕显示 Temp: 45.3C"

## Error handling

- LED 是高电平点亮（active-high），工具内部已处理极性，无需关心
- 传感器未接时返回 error 并注明哪个设备未检测到（如 "BH1750 not detected on I2C1"）
- 参数越界自动钳位（duty>100 → 100）
