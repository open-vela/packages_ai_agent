/****************************************************************************
 * Launcher Page - Application Desktop
 *
 * Provides a desktop-like UI with app icons that can be clicked to enter
 * different application pages (pet display, settings, about, etc.).
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netutils/netlib.h>
#include "launcher_page.h"
#include "pet_page.h"
#include "settings_page.h"
#include "about_page.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LAUNCHER_TITLE_TEXT  "HerSen. Welcome back."

/* Bluetooth PAN is the watch's only uplink, and the interface carries an
 * address only once BNEP is up and DHCP has answered.
 */

#define LAUNCHER_NET_IFNAME  "bt-pan"
#define LAUNCHER_NET_PERIOD  2000

/* Polls without an address before we stop saying "connecting" and start
 * telling the user what to actually go and do.  10 x 2 s = 20 s, comfortably
 * longer than a healthy pair + BNEP + DHCP sequence (~8 s measured). */
#define LAUNCHER_NET_GRACE_POLLS 10

#define CLR_STATUS_OK   0xffffff
#define CLR_STATUS_WAIT 0x9a9ab0
#define CLR_STATUS_ERR  0xffc06a

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Background wallpaper (390x450 RGB565, see tools/gen_bg_rgb565.py) */
extern const uint16_t img_background_watch_px[];

/* Sans-serif (Noto Sans SC) font family: CJK + Latin, multi sizes/weights */
extern const lv_font_t ui_font_sans_16;
extern const lv_font_t ui_font_sans_24;
extern const lv_font_t ui_font_sans_32;
extern const lv_font_t ui_font_sans_64;
extern const lv_font_t ui_font_sans_72;
extern const lv_font_t ui_font_sans_88;
extern const lv_font_t ui_font_sans_88_bold;

static const lv_image_dsc_t s_bg_dsc = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_RGB565,
                .flags = 0,
                .w = 390,
                .h = 450,
                .stride = 390 * 2,
                .reserved_2 = 0 },
    .data_size = 390 * 450 * 2,
    .data = (const uint8_t *)img_background_watch_px,
    .reserved = NULL,
    .reserved_2 = NULL,
};

static lv_obj_t *desktop_page;
static lv_obj_t *pet_icon_btn;
static lv_obj_t *settings_icon_btn;
static lv_obj_t *about_icon_btn;
static lv_obj_t *s_time_label;
static lv_obj_t *s_date_label;
static int s_shown_day_key = -1; /* year+yday the date label currently shows */
static char s_shown_time[8];      /* "HH:MM" the clock label currently shows */
static lv_obj_t *s_title_label;
static lv_obj_t *s_status_label;  /* second line: IP, or why there is none */
static char s_shown_status[64];   /* text the status label currently shows */
static int s_no_ip_polls;         /* consecutive polls with no address */
static lv_obj_t *s_current_page; /* page currently stacked above the desktop */

/****************************************************************************
 * Icon Click Callbacks
 ****************************************************************************/

/**
 * Pet icon clicked - enter pet display page
 */
static void on_pet_icon_clicked(lv_event_t *e)
{
    printf("[Launcher] Pet icon clicked - entering pet page\n");
    launcher_enter_page(PAGE_PET);
}

/**
 * Settings icon clicked - enter settings page
 */
static void on_settings_icon_clicked(lv_event_t *e)
{
    printf("[Launcher] Settings icon clicked - entering settings page\n");
    launcher_enter_page(PAGE_SETTINGS);
}

/**
 * About icon clicked - enter about page
 */
static void on_about_icon_clicked(lv_event_t *e)
{
    printf("[Launcher] About icon clicked - entering about page\n");
    launcher_enter_page(PAGE_ABOUT);
}

/****************************************************************************
 * Clock
 ****************************************************************************/

/**
 * Update the desktop clock and the date stack from the system RTC.
 * Runs every second via lv_timer; also called once at build time.
 *
 * The date used to be the literal "WED\nAPR\n1" -- harmless while the RTC held
 * an arbitrary value, but wrong now that time_sync.c sets a real one.  Both
 * labels are refreshed from one localtime_r() call; the date only touches the
 * label when the day actually changes, so a per-second timer costs nothing.
 *
 * Weekday and month names are spelled out here rather than taken from
 * strftime("%a"/"%b"): those are uppercase in this design, and the build has no
 * locale support to lean on anyway.
 */
static void time_update_cb(lv_timer_t *timer)
{
    static const char *const wday[7] = {
        "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
    };
    static const char *const month[12] = {
        "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
        "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"
    };
    time_t now;
    struct tm tmv;
    char buf[8];
    char date[16];
    int day_key;

    (void)timer;
    if (s_time_label == NULL) {
        return;
    }
    now = time(NULL);
    if (localtime_r(&now, &tmv) == NULL) {
        return;
    }

    /* Only write when the minute actually rolls over.  lv_label_set_text
     * invalidates unconditionally, and an invalidation costs a full render
     * pass -- there is no point paying it 59 times out of every 60. */

    snprintf(buf, sizeof(buf), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    if (strcmp(buf, s_shown_time) != 0) {
        lv_label_set_text(s_time_label, buf);
        strlcpy(s_shown_time, buf, sizeof(s_shown_time));
    }

    /* Key on the year too, not just the day of year: the clock jumps when
     * time_sync.c lands, and a jump that keeps tm_yday but changes the year
     * lands on a different weekday. */

    day_key = tmv.tm_year * 1000 + tmv.tm_yday;
    if (s_date_label != NULL && day_key != s_shown_day_key &&
        tmv.tm_wday >= 0 && tmv.tm_wday < 7 &&
        tmv.tm_mon >= 0 && tmv.tm_mon < 12) {
        snprintf(date, sizeof(date), "%s\n%s\n%d",
                 wday[tmv.tm_wday], month[tmv.tm_mon], tmv.tm_mday);
        lv_label_set_text(s_date_label, date);
        s_shown_day_key = day_key;
    }
}

/****************************************************************************
 * Network status
 ****************************************************************************/

/**
 * Second desktop line: the bt-pan address once we have one, otherwise why we
 * do not.
 *
 * Runs every LAUNCHER_NET_PERIOD ms via lv_timer, and once at build time so a
 * board that is already online shows its address immediately.  The text is
 * only written when it changes -- lv_label_set_text invalidates, and an
 * invalidation costs a render pass.
 *
 * The three states are all we can tell apart cheaply from the LVGL thread, and
 * that is enough to be useful: the overwhelmingly common reason for no address
 * is that Bluetooth tethering got switched off on the phone (Android turns it
 * off by itself after a reboot or a spell in airplane mode).  The link comes up
 * and BNEP negotiates fine in that case -- the phone tears it down the moment
 * DHCP starts -- so "no IP for a while" is exactly the signal to surface.
 *
 * This label uses ui_font_sans_16, not the 24 px title font: sans_24 is a
 * 117-glyph subset carrying only the fixed strings already on screen, so any
 * new Chinese would render as boxes.  sans_16 carries the full CJK subset and
 * is already linked in for the footer.
 */
static void net_update_cb(lv_timer_t *timer)
{
    struct in_addr addr;
    char ip[INET_ADDRSTRLEN];
    const char *text;

    (void)timer;
    if (s_status_label == NULL) {
        return;
    }

    ip[0] = '\0';
    memset(&addr, 0, sizeof(addr));
    if (netlib_get_ipv4addr(LAUNCHER_NET_IFNAME, &addr) == 0 &&
        addr.s_addr != INADDR_ANY) {
        if (inet_ntop(AF_INET, &addr, ip, sizeof(ip)) == NULL) {
            ip[0] = '\0';
        }
    }

    if (ip[0] != '\0') {
        s_no_ip_polls = 0;
        text = ip;
        lv_obj_set_style_text_color(s_status_label,
                                    lv_color_hex(CLR_STATUS_OK), 0);
    }
    else {
        if (s_no_ip_polls < LAUNCHER_NET_GRACE_POLLS) {
            s_no_ip_polls++;
        }
        if (s_no_ip_polls < LAUNCHER_NET_GRACE_POLLS) {
            /* 蓝牙连接中… */
            text = "\xe8\x93\x9d\xe7\x89\x99\xe8\xbf\x9e\xe6\x8e\xa5"
                   "\xe4\xb8\xad\xe2\x80\xa6";
            lv_obj_set_style_text_color(s_status_label,
                                        lv_color_hex(CLR_STATUS_WAIT), 0);
        }
        else {
            /* 蓝牙未连接，请开启手机网络共享 */
            text = "\xe8\x93\x9d\xe7\x89\x99\xe6\x9c\xaa\xe8\xbf\x9e"
                   "\xe6\x8e\xa5\xef\xbc\x8c\xe8\xaf\xb7\xe5\xbc\x80"
                   "\xe5\x90\xaf\xe6\x89\x8b\xe6\x9c\xba\xe7\xbd\x91"
                   "\xe7\xbb\x9c\xe5\x85\xb1\xe4\xba\xab";
            lv_obj_set_style_text_color(s_status_label,
                                        lv_color_hex(CLR_STATUS_ERR), 0);
        }
    }

    if (strcmp(text, s_shown_status) == 0) {
        return;
    }
    strlcpy(s_shown_status, text, sizeof(s_shown_status));
    lv_label_set_text(s_status_label, text);
    syslog(LOG_INFO, "[launcher] status: %s\n", text);
}

/****************************************************************************
 * Icon Creation Helper
 ****************************************************************************/

/**
 * Create an app icon button with emoji label
 *
 * @param parent Parent container
 * @param emoji Icon emoji/unicode character
 * @param label Button label text
 * @param x X position
 * @param y Y position
 * @return Icon button object
 */
static lv_obj_t *create_icon_button(lv_obj_t *parent, const char *emoji,
                                     const char *label, lv_coord_t x, lv_coord_t y)
{
    (void)x;
    (void)y;
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 60, 60); /* 60x60 圆形按钮 */
    /* 位置由父容器 flex 布局决定（不再使用绝对坐标） */
    //bg：白色 50% 透明
    lv_obj_set_style_bg_color(btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_50, LV_STATE_DEFAULT);
    // 去掉所有状态的边框
    lv_obj_set_style_border_width(btn, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_border_width(btn, 0, LV_STATE_FOCUS_KEY);
    //radius：全圆角（圆形按钮）
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);

    /* Button label */
    lv_obj_t *text_label = lv_label_create(btn);
    lv_label_set_text(text_label, label);
    lv_obj_set_style_text_font(text_label, &ui_font_sans_24, 0);
    lv_obj_set_style_text_color(text_label, lv_color_hex(0xffffff), 0);
    lv_obj_align(text_label, LV_ALIGN_CENTER, 0, 0);

    return btn;
}

/****************************************************************************
 * Public API
 ****************************************************************************/

/**
 * Create the launcher desktop page
 *
 * This creates the main desktop with app icons. Call this first in ui_build().
 */
void launcher_create(void)
{
    printf("[Launcher] Creating desktop page...\n");

    /* Create desktop container on active screen */

    desktop_page = lv_obj_create(lv_scr_act());
    lv_obj_set_size(desktop_page, 390, 450);
    lv_obj_set_pos(desktop_page, 0, 0);
    lv_obj_set_style_bg_color(desktop_page, lv_color_hex(0x1a1a2e), 0); /* Dark blue */
    /* Wallpaper background */
    lv_obj_set_style_bg_image_src(desktop_page, &s_bg_dsc, 0);
    lv_obj_set_style_bg_image_opa(desktop_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(desktop_page, 0, 0);
    lv_obj_set_style_pad_all(desktop_page, 20, 0); /* 左右 20 */
    lv_obj_set_style_pad_top(desktop_page, 10, 0);    /* 上内边距 10 */
    lv_obj_set_style_pad_bottom(desktop_page, 10, 0); /* 下内边距 10（与上部一致） */

    /* 整体 column 布局 */
    lv_obj_set_layout(desktop_page, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(desktop_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(desktop_page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* ── 第一层：row 布局，占 30%，元素 space-between，取消内边距 ──
     * 三栏比例计算（内容区高 = 450 - 10(上) - 10(下) = 430）：
     *   row   30% -> 129px（y 10..139）
     *   mid   flex-grow（剩余 40% ≈ 172px，y 139..311，中心 225 = 屏幕中心）
     *   col3  30% -> 129px（y 311..440，60x60 按钮 + footer 不拥挤） */
    lv_obj_t *row = lv_obj_create(desktop_page);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, lv_pct(30));
    lv_obj_set_style_margin_top(row, 10, 0); /* 第一层上外距离 +10（距窗口顶部更远） */
    lv_obj_set_style_bg_opa(row, LV_OPA_0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(row, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(row, 0, 0); /* 取消内变距 */
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 左侧日期（星期 / 月份 / 日，随 RTC 走，见 time_update_cb） */
    s_date_label = lv_label_create(row);
    s_shown_day_key = -1; /* new label object: repaint on the next tick */
    lv_label_set_text(s_date_label, "---\n---\n-");
    lv_obj_set_style_text_font(s_date_label, &ui_font_sans_24, 0);
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(s_date_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(s_date_label, 2, 0);

    /* 右侧时间（实时时钟，粗体） */
    s_time_label = lv_label_create(row);
    s_shown_time[0] = '\0';   /* new label object: repaint on the next tick */
    lv_label_set_text(s_time_label, "--:--");
    lv_obj_set_style_text_font(s_time_label, &ui_font_sans_88_bold, 0);
    lv_obj_set_style_text_color(s_time_label, lv_color_hex(0xffffff), 0);
    time_update_cb(NULL);
    lv_timer_create(time_update_cb, 1000, NULL);

    /* ── 第二层：中间栏（flex-grow 占满剩余空间），标题绝对居中于屏幕 ── */
    lv_obj_t *mid = lv_obj_create(desktop_page);
    lv_obj_set_width(mid, lv_pct(100));
    lv_obj_set_style_bg_opa(mid, LV_OPA_0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(mid, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(mid, 0, 0);
    lv_obj_set_flex_grow(mid, 1);

    lv_obj_t *title = lv_label_create(mid);
    lv_label_set_text(title, LAUNCHER_TITLE_TEXT);
    lv_obj_set_style_text_font(title, &ui_font_sans_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -14); /* 标题略上移，给状态行让位 */
    s_title_label = title;

    /* 状态行：有地址显示 IP，没有就说明原因。用 sans_16（含完整 CJK 子集） */
    s_status_label = lv_label_create(mid);
    lv_label_set_text(s_status_label, "");
    lv_obj_set_style_text_font(s_status_label, &ui_font_sans_16, 0);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(CLR_STATUS_WAIT), 0);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 16);

    s_shown_status[0] = '\0';
    s_no_ip_polls = 0;
    net_update_cb(NULL);
    lv_timer_create(net_update_cb, LAUNCHER_NET_PERIOD, NULL);

    /* ── 第三层：column 布局，占 30% ── */
    lv_obj_t *col3 = lv_obj_create(desktop_page);
    lv_obj_set_width(col3, lv_pct(100));
    lv_obj_set_height(col3, lv_pct(30));
    lv_obj_set_style_bg_opa(col3, LV_OPA_0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(col3, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(col3, 0, 0);
    lv_obj_set_layout(col3, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(col3, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col3, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* btngroup：row 布局，space-between，flex-grow 占满第三层剩余空间 */
    lv_obj_t *btngroup = lv_obj_create(col3);
    lv_obj_set_width(btngroup, lv_pct(100));
    lv_obj_set_style_bg_opa(btngroup, LV_OPA_0, LV_STATE_DEFAULT);
    lv_obj_set_style_border_width(btngroup, 0, LV_STATE_DEFAULT);
    lv_obj_set_style_pad_all(btngroup, 0, 0);
    lv_obj_set_flex_grow(btngroup, 1); /* 按钮组占满剩余空间，按钮垂直居中 */
    lv_obj_set_layout(btngroup, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btngroup, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btngroup, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* 按钮顺序：设置（左）、桌宠（中，全宽）、关于（右）
     * 尺寸计算（btngroup 内容宽 = 390 - 20*2 = 350）：
     *   设置 60 + 间隙 20 + 桌宠 190 + 间隙 20 + 关于 60 = 350
     *   → 桌宠宽度 = 350 - 60 - 60 - 40 = 190（space-between 自动分配 2×20 间隙）
     *   不用 flex-grow：与 space-between 叠加在 LVGL 中不可靠，会溢出屏幕 */
    settings_icon_btn = create_icon_button(btngroup, "◉", "设置", 0, 0);
    lv_obj_add_event_cb(settings_icon_btn, on_settings_icon_clicked,
                        LV_EVENT_CLICKED, NULL);

    pet_icon_btn = create_icon_button(btngroup, "☁", "桌宠", 0, 0);
    lv_obj_set_width(pet_icon_btn, 190); /* 精确计算的全宽（胶囊形） */
    lv_obj_add_event_cb(pet_icon_btn, on_pet_icon_clicked,
                        LV_EVENT_CLICKED, NULL);

    about_icon_btn = create_icon_button(btngroup, "★", "关于", 0, 0);
    lv_obj_add_event_cb(about_icon_btn, on_about_icon_clicked,
                        LV_EVENT_CLICKED, NULL);

    /* 一行字在屏幕最底下（不参与 grow，跟在按钮组之后即底部） */
    lv_obj_t *footer = lv_label_create(col3);
    lv_label_set_text(footer, "powered by AIIRA");
    lv_obj_set_style_text_font(footer, &ui_font_sans_16, 0);
    lv_obj_set_style_text_color(footer, lv_color_hex(0xffffff), 0);
    lv_obj_set_width(footer, lv_pct(100));
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);

    printf("[Launcher] Desktop created successfully\n");
}

/**
 * Get the desktop page object
 */
lv_obj_t *launcher_desktop_obj(void)
{
    return desktop_page;
}

/**
 * Enter a specific application page
 *
 * @param page Page ID (PAGE_PET, PAGE_SETTINGS, PAGE_ABOUT)
 */
void launcher_enter_page(launcher_page_id_t page)
{
    lv_obj_t *target_page = NULL;

    switch (page)
    {
        case PAGE_PET:
            printf("[Launcher] Entering pet page\n");
            target_page = pet_page_create();
            if (target_page == NULL)
            {
                printf("[Launcher] ERROR: Failed to create pet page\n");
                return;
            }
            pet_page_set_back_callback(launcher_back_to_desktop);
            break;

        case PAGE_SETTINGS:
            printf("[Launcher] Entering settings page\n");
            target_page = settings_page_create();
            if (target_page == NULL)
            {
                printf("[Launcher] ERROR: Failed to create settings page\n");
                return;
            }
            settings_page_set_back_callback(launcher_back_to_desktop);
            break;

        case PAGE_ABOUT:
            printf("[Launcher] Entering about page\n");
            target_page = about_page_create();
            if (target_page == NULL)
            {
                printf("[Launcher] ERROR: Failed to create about page\n");
                return;
            }
            about_page_set_back_callback(launcher_back_to_desktop);
            break;

        default:
            printf("[Launcher] Unknown page ID: %d\n", page);
            return;
    }

    if (target_page != NULL)
    {
        s_current_page = target_page;
        lv_obj_move_foreground(target_page);
    }
}

/**
 * Delete only the current page stacked above the desktop. Runs on the
 * next LVGL safe point (via lv_async_call) so we never destroy an object
 * that is still processing its own press/click event.
 *
 * NOTE: only s_current_page is removed - the pet/bubble/menu stage objects
 * created by lvgl_ui_channel must stay alive, otherwise their timers and
 * animations keep referencing deleted widgets and the UI hangs.
 */
static void destroy_pages_async_cb(void *data)
{
    (void)data;
    if (s_current_page != NULL)
    {
        lv_obj_del(s_current_page);
        s_current_page = NULL;
    }

    /* Ensure desktop is in foreground */

    lv_obj_move_foreground(desktop_page);
}

/**
 * Return to desktop from current page
 */
void launcher_back_to_desktop(void)
{
    printf("[Launcher] Returning to desktop\n");
    lv_async_call(destroy_pages_async_cb, NULL);
}

/**
 * Check if currently on desktop
 *
 * @return true if on desktop page, false otherwise
 */
bool launcher_is_on_desktop(void)
{
    lv_obj_t *current = lv_screen_active();
    return (current == desktop_page);
}
