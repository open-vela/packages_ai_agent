/****************************************************************************
 * settings_page.c - 设置页（分组列表）
 *
 *   ┌──────────────────────────┐
 *   │ 设置              --  🔋 │  状态栏 32px（与其他页一致）
 *   ├──────────────────────────┤
 *   │ 显示与提醒                │  18px 分组标题 21px
 *   │  亮度              100%   │
 *   │  音量               70%   │  列表行 48px（原 54）
 *   │  勿扰模式            关    │
 *   │ 连接与 AI                 │
 *   │  蓝牙           已连接     │
 *   │  AI 配置        未配置     │
 *   │  息屏           轻触灭屏    │
 *   └──────────────────────────┘
 *
 * 改动要点（对照旧版）：
 *   1. 标题从自绘的 24px label 换成 ui_statusbar_create()，与运动/天气/
 *      闹钟/休息页一致；原来自绘标题既比其他页低一档，又多占 29px
 *   2. 行高 54 -> 48、pad_row 8 -> 6；两个 18px 分组标题把 6 行分成
 *      「显示与提醒」「连接与 AI」，息屏是一次性动作，单独放最后
 *   3. 高度账：32(状态栏) + 2×21(分组标题) + 6×48(行) + 8×6(行距) = 410，
 *      正好顶满 410px 内容区，最后一行离屏底只剩 20px；收掉分组标题
 *      下方那两段 6px 行距后总高 398，底部留 32px，不蹭圆角
 *   4. 颜色全部走 ui_theme.h 令牌；AI 配置仍只读
 *
 * DEMO-PHONE（2026-11-03）：手表端「演示与测试」分组已移除 —— 演示视频里
 * 所有主动关怀改由手机 App（BLE demo 命令）触发，手表只负责切到小云页 +
 * 气泡 + 本地预合成语音；串口调试仍可用 NSH 的 ui demo 命令。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <nuttx/lcd/lcd_dev.h>   /* LCDDEVIO_SETCONTRAST / SETPOWER */

#include "settings_page.h"
#include "ui_nav.h"          /* ui_nav_attach_back_swipe（消除隐式声明告警） */
#include "pet_care.h"          /* pet_care_set_dnd / get_dnd */
#include "ui_gesture.h"        /* 防误触 */
#include "ui_theme.h"
#include "ui_text_runtime.h"
#include "lvgl_ui_channel.h"   /* lvgl_ui_post：非 render 线程异步刷标签 */

/****************************************************************************
 * 私有数据
 ****************************************************************************/

static lv_obj_t *s_page;
static lv_obj_t *s_llm_host_label;
static lv_obj_t *s_llm_model_label;
static lv_obj_t *s_llm_key_label;
static pet_back_callback_t s_back_callback;

static lv_obj_t *s_bright_label;
/* 档位索引：UI 线程与 BLE RX 线程都会读写（settings_page_brightness_*）。
 * 单字读写本身原子，volatile 只为避免编译器把旧值缓存在寄存器里。 */
static volatile int s_bright_idx = 4;   /* 默认 100%，档位下标 0..4 */

static lv_obj_t *s_dnd_label;

static void dnd_show(void)
{
    if (s_dnd_label == NULL)
      {
        return;
      }

    lv_label_set_text(s_dnd_label, pet_care_get_dnd() ? "开" : "关");
    lv_obj_set_style_text_color(s_dnd_label,
                                lv_color_hex(pet_care_get_dnd()
                                             ? UI_ACCENT
                                             : UI_TEXT_SECONDARY), 0);
}

static void on_dnd_clicked(lv_event_t *e)
{
    (void)e;

    if (ui_gesture_was_drag())
      {
        return;
      }

    pet_care_set_dnd(!pet_care_get_dnd());
    dnd_show();
}

/****************************************************************************
 * 亮度 / 息屏
 *
 * 这两项以前在界面上只是**写死的静态标签**（"中" / "关"），点了没反应。
 * 底下其实一直有通路：
 *   ioctl(/dev/lcd0, LCDDEVIO_SETCONTRAST, 0..CONFIG_LCD_MAXCONTRAST)
 *   ioctl(/dev/lcd0, LCDDEVIO_SETPOWER,    0=灭屏 / 1=亮屏)
 * 只是驱动侧的 setcontrast 原来直接 return -ENOSYS —— 这一版已经把它接到
 * co5300.c 里那个一直没人调用的 LCD_SetBrightness() 上了。
 ****************************************************************************/

static const int s_bright_levels[] = { 20, 40, 60, 80, 100 };

static void lcd_ioctl(unsigned long cmd, unsigned long arg)
{
    int fd = open("/dev/lcd0", O_RDWR);

    if (fd < 0)
      {
        return;
      }

    (void)ioctl(fd, cmd, arg);
    close(fd);
}

static void brightness_apply(int pct)
{
    unsigned int contrast =
        (unsigned int)((long)pct * CONFIG_LCD_MAXCONTRAST / 100);

    lcd_ioctl(LCDDEVIO_SETCONTRAST, (unsigned long)contrast);
}

static void brightness_show(void)
{
    char buf[16];
    int pct = s_bright_levels[s_bright_idx];

    if (s_bright_label == NULL)
      {
        return;
      }

    snprintf(buf, sizeof(buf), "%d%%", pct);
    lv_label_set_text(s_bright_label, buf);
}

/* ── 息屏 ────────────────────────────────────────────────────────
 *
 * 灭屏本身很简单（LCDDEVIO_SETPOWER 传 0），麻烦的是**怎么亮回来**。
 * 如果只是点了就灭，用户在黑屏上乱点却唤不醒，就等于把设备锁死了。
 *
 * 做法：
 *   - 灭屏后置 s_blanked，并在 800ms 内忽略输入（否则"熄灭"这一下点击
 *     会立刻冒泡到页面把屏幕又点亮，等于点了没用）
 *   - 800ms 之后，页面上任何一次点击/按下都先亮屏，并把这次事件吞掉
 *     （不触发下面的按钮，避免"亮屏那一下误点到某个功能"）
 */
#define WAKE_GUARD_MS  800

static volatile int      s_blanked;
static volatile uint32_t s_blanked_at;

static void screen_set_power(int on)
{
    lcd_ioctl(LCDDEVIO_SETPOWER, on ? 1 : 0);
}

/* 任何一次触摸先看要不要唤醒。返回 1 表示"这次事件被唤醒动作吃掉了"。 */
static int wake_if_blanked(void)
{
    if (!s_blanked)
      {
        return 0;
      }

    /* 冷却期内不唤醒：否则"点息屏"那一下会立刻冒泡到这里又点亮 */
    if ((uint32_t)(lv_tick_get() - s_blanked_at) < WAKE_GUARD_MS)
      {
        return 1;
      }

    s_blanked = 0;
    screen_set_power(1);
    return 1;
}

/* ★ 2026-10-25 P17 保险：给物理按键用的公开唤醒接口。
 *
 * 背景：息屏后的唯一唤醒路径原本是触摸。但触摸 FT6146 的 I2C 会偶发
 * 锁死（实测 dd /dev/input0 永久阻塞、reset 无效、普通插拔无效，只有
 * 30s+ 长断电能救）—— 触摸一死，息屏状态就等于把设备永久锁在黑屏。
 * KEY1 走 GPIO 轮询不经过 I2C，是触摸死锁时唯一可靠的逃生口。
 * 调用方（key_input.c）在任何按键动作前先调这里；返回 true 表示
 * "这一下用于唤醒屏幕了"，调用方应吞掉本次按键不执行原动作。 */
bool settings_page_wake_screen(void)
{
    if (!s_blanked)
      {
        return false;
      }

    if ((uint32_t)(lv_tick_get() - s_blanked_at) < WAKE_GUARD_MS)
      {
        return true;   /* 冷却期内：算作已处理（吞掉），但不点亮 */
      }

    s_blanked = 0;
    screen_set_power(1);
    printf("[Settings] screen woken by physical key\n");
    return true;
}

static void on_sleep_clicked(lv_event_t *e)
{
    (void)e;

    /* ★ 这里最危险：误触 = 直接黑屏，用户会以为死机了 */
    if (ui_gesture_was_drag())
      {
        return;
      }

    s_blanked_at = lv_tick_get();
    s_blanked = 1;
    screen_set_power(0);
}

/* 页面级：任何时候被按下，先处理唤醒 */
static void on_page_pressed(lv_event_t *e)
{
    if (wake_if_blanked())
      {
        lv_event_stop_bubbling(e);
      }
}

/* 点一下切一档：20 → 40 → 60 → 80 → 100 → 20 … */
static void on_brightness_clicked(lv_event_t *e)
{
    (void)e;

    /* 防误触：从这一行上开始上下滑不该改亮度 */
    if (ui_gesture_was_drag())
      {
        return;
      }

    s_bright_idx = (s_bright_idx + 1) %
                   (int)(sizeof(s_bright_levels) / sizeof(s_bright_levels[0]));

    brightness_apply(s_bright_levels[s_bright_idx]);
    brightness_show();
}

/****************************************************************************
 * 列表行
 ****************************************************************************/

static lv_obj_t *row_create(lv_obj_t *parent, const char *name,
                            const char *value)
{
    lv_obj_t *row = lv_obj_create(parent);
    /* 48 行高：24px 文字（line_height 29）上下各留约 9px，点击目标仍够；
     * 6 行共 288，加 32(状态栏) + 42(两个分组标题) + 48(行距) = 410 刚好
     * 排满 410px 内容区，所以分组标题再各收 6px，把总高压到 398 留出底部。 */
    lv_obj_set_size(row, lv_pct(100), 48);

    /* Apple Watch 的设置列表：无卡片底，只有行底一条细分隔线 */
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(row, 1, 0);
    lv_obj_set_style_border_color(row, lv_color_hex(UI_DIVIDER), 0);
    lv_obj_set_style_pad_hor(row, 4, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *lbl = lv_label_create(row);
    lv_label_set_text(lbl, name);
    lv_obj_set_style_text_font(lbl, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT_PRIMARY), 0);

    lv_obj_t *right = lv_label_create(row);
    lv_obj_set_style_text_font(right, UI_FONT_BODY, 0);
    lv_obj_set_style_text_color(right, lv_color_hex(UI_TEXT_SECONDARY), 0);
    lv_label_set_text(right, value != NULL ? value : "");

    return right;
}

/* 分组标题：新增的中文文案一律 18px —— 24px 子集只有 349 个汉字，
 * 「显示与提醒」里的「与」「提」就不在里面，用 24px 会掉字。
 * 三级灰。高度钉死 21（= 18px 字体的 line_height），整页高度账才算得准。
 * margin_bottom = -6：标题和它下面第一行之间不再额外留行距 —— 否则
 * 32 + 2×21 + 6×48 + 8×6 = 410 正好顶满内容区，最后一行离屏底只剩 20px
 * （旧版 23px 已经被反馈"贴圆角"）。收掉 2×6px 后总高 398，底部留 32px。 */
static lv_obj_t *group_title_create(lv_obj_t *parent, const char *text)
{
    lv_obj_t *lbl = lv_label_create(parent);

    lv_obj_set_size(lbl, lv_pct(100), 21);
    lv_obj_set_style_pad_all(lbl, 0, 0);
    lv_obj_set_style_pad_left(lbl, 4, 0);   /* 与列表行文字的左边距对齐 */
    lv_obj_set_style_text_font(lbl, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(UI_TEXT_TERTIARY), 0);
    lv_obj_set_style_margin_bottom(lbl, -6, 0);
    lv_label_set_text(lbl, text);

    return lbl;
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

/* ── 亮度档位 API（98 号文 FW-B）─────────────────────────────────
 *
 * BLE settings_get / settings_set 需要「当前档位 / 设档位 / 最大档位」。
 * 档位就是 s_bright_levels 的下标（0..4 ↔ 20%..100%），与设置页
 * 「亮度」那一行显示的是同一个值 —— App 侧以 UI 档位为准，不再引入
 * 第二套亮度状态（s_bright_idx 初值 4 即使与上电时面板实际亮度不同，
 * 也以 UI 档位回答，避免 App 与手表显示打架）。
 */

/* BLE RX 线程里不能直接碰 LVGL（LV_OS_NONE，见 lvgl_ui_channel.h 注释），
 * 所以标签刷新一律投递回 render 线程；硬件亮度在调用线程当场生效。 */
static void brightness_show_async_cb(void *arg)
{
    (void)arg;
    brightness_show();
}

int settings_page_brightness_max(void)
{
    return (int)(sizeof(s_bright_levels) / sizeof(s_bright_levels[0])) - 1;
}

int settings_page_brightness_get(void)
{
    return s_bright_idx;
}

int settings_page_brightness_set(int level)
{
    int max = settings_page_brightness_max();

    /* 越界钳位：App 传负值或超过 max 都不能让下标跑出数组 */
    if (level < 0)
      {
        level = 0;
      }
    else if (level > max)
      {
        level = max;
      }

    s_bright_idx = level;
    brightness_apply(s_bright_levels[level]);   /* 复用现有硬件通路 */
    (void)lvgl_ui_post(brightness_show_async_cb, NULL);

    printf("[Settings] brightness=%d/%d (%d%%)\n",
           level, max, s_bright_levels[level]);
    return level;
}

lv_obj_t *settings_page_create(void)
{
    s_page = lv_obj_create(lv_scr_act());
    ui_page_init(s_page);

    /* 不额外套面板：列表行直接排在页面底色上。
     * 多套一层会把可用宽高再缩一圈，纵向立刻溢出（旧版实测）。 */
    lv_obj_t *sheet = s_page;
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sheet, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(sheet, 6, 0);

    /* 标题交给状态栏：运动/天气/闹钟/休息页都走这个入口，
     * 原来自绘的 24px 标题既多占 29px，又比其他页低一档。 */
    (void)ui_statusbar_create(s_page, "设置");

    /* ── 显示与提醒：亮度 / 音量 / 勿扰 ────────────────────── */
    group_title_create(s_page, "显示与提醒");

    /* 亮度：做成可点的行，点一下切一档 */
    s_bright_label = row_create(s_page,
                                ui_str("settings.brightness", "亮度"), "100%");
    {
        lv_obj_t *row = lv_obj_get_parent(s_bright_label);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_brightness_clicked, LV_EVENT_CLICKED, NULL);
    }
    brightness_show();

    /* 标签写的是 100%，面板也必须真的是 100%，否则第一次点会从"出厂默认"
     * 直接掉到 20%，用户会以为屏幕坏了。 */
    brightness_apply(s_bright_levels[s_bright_idx]);
    row_create(s_page, ui_str("settings.volume", "音量"), ui_str("settings.volume.v", "70%"));
    /* 勿扰模式：点一下切换。开启后主动关怀不再产生（闹钟仍照常响）。 */
    s_dnd_label = row_create(s_page, ui_str("settings.dnd", "勿扰模式"), "关");
    {
        lv_obj_t *row = lv_obj_get_parent(s_dnd_label);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_dnd_clicked, LV_EVENT_CLICKED, NULL);
    }
    dnd_show();

    /* ── 连接与 AI：蓝牙 / AI 配置 ─────────────────────────── */
    group_title_create(s_page, "连接与 AI");

    row_create(s_page, ui_str("settings.bluetooth", "蓝牙"), ui_str("settings.bluetooth.v", "已连接"));

    /* LLM 配置只读：手表屏幕上一行就够，显示当前状态，
     * 详细信息去手机 App 或 NSH 看。分开三行会把列表撑出屏幕（旧版实测
     * 被截断），所以这里仍然只有一个值标签，也就是 update_host 的落点。 */
    s_llm_host_label = row_create(s_page, ui_str("settings.ai", "AI 配置"), ui_str("settings.ai.v", "未配置"));
    s_llm_model_label = NULL;
    s_llm_key_label = NULL;

    /* 息屏：一次性动作，不属于任何分组，单独放最后一行。
     * 点一下灭屏，之后任意触摸/物理按键唤醒（见 on_page_pressed 的说明）。 */
    {
        lv_obj_t *right = row_create(s_page,
                                     ui_str("settings.sleep", "息屏"), "轻触灭屏");
        lv_obj_t *row = lv_obj_get_parent(right);

        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_sleep_clicked, LV_EVENT_CLICKED, NULL);
    }

    /* 页面级唤醒监听。用 PRESSED 而不是 CLICKED：灭屏后用户第一下多半
     * 只是摸一下，不一定会完整"按下-抬起"，PRESSED 更早、更可靠。
     * 注册在所有行之后：事件从行冒泡到页面，与行创建顺序无关。 */
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_page, on_page_pressed, LV_EVENT_PRESSED, NULL);

    printf("[Settings] created\n");
    ui_nav_attach_back_swipe(s_page);

    return s_page;
}

void settings_page_set_back_callback(pet_back_callback_t callback)
{
    s_back_callback = callback;
}

void settings_page_update_host(const char *host)
{
    if (s_llm_host_label != NULL && host != NULL)
      {
        lv_label_set_text(s_llm_host_label, host);
      }
}

void settings_page_update_model(const char *model)
{
    if (s_llm_model_label != NULL && model != NULL)
      {
        lv_label_set_text(s_llm_model_label, model);
      }
}

void settings_page_update_key(const char *key)
{
    if (s_llm_key_label == NULL || key == NULL)
      {
        return;
      }

    if (strlen(key) > 4)
      {
        char masked[32];

        snprintf(masked, sizeof(masked), "%.4s****", key);
        lv_label_set_text(s_llm_key_label, masked);
      }
    else
      {
        lv_label_set_text(s_llm_key_label, "****");
      }
}

void settings_page_delete(void)
{
    if (s_page != NULL)
      {
        lv_obj_delete(s_page);
        s_page = NULL;
      }

    s_llm_host_label = NULL;
    s_llm_model_label = NULL;
    s_llm_key_label = NULL;
    /* 亮度/勿扰标签随页面对象一起被删掉，必须同步清空：亮度标签会被
     * 异步回调（brightness_show_async_cb）在页面删掉之后才刷新，
     * 留着悬空指针就是踩已释放对象。 */
    s_bright_label = NULL;
    s_dnd_label = NULL;
    s_back_callback = NULL;
}
