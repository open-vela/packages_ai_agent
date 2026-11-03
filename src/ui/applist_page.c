/****************************************************************************
 * applist_page.c - 应用入口（2x3 宫格，两页）
 *
 * 2026-11-02（批次 2）：从 Apple Watch 蜂窝气泡网格改成 2x3 宫格。
 *
 * 为什么改：
 *   1. 这块屏 390px 宽，蜂窝边缘气泡只有 74% 缩放，图标下**放不下文字标签**
 *      —— 用户得靠背图案找入口（"关于""闹钟"这两个连图案都没有，只有字母）。
 *      docs_ble/92 号方案把"图标 + 文字标签"定为可用性目标。
 *   2. 参考的 Lumina 设计稿就是 2x3 大触控块 + 下方标签，6 个/页、上滑翻页。
 *   3. 每个格子 104x82 + 标签，触控区远大于 60x60 下限（UI_TAP_MIN）。
 *
 * 与设计稿的差异（有意为之）：
 *   - 设计稿在**卡片内**和**卡片下**各写一遍应用名（重复且只有"小云"格不
 *     一致，见 docs_ble/91c P2-6）。这里只保留卡片下的标签。
 *   - 设计稿把图标直接当卡片背景色块，这里改成浅色卡片 + 彩色图标，
 *     深色文字始终压在浅底上（对比度稳定 >12:1）。
 *
 * 蜂窝实现在 bubble_grid.c 里保留未删（配 docs_ble 历史记录），只是不再被本页使用。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <string.h>

#include "applist_page.h"
#include "ui_theme.h"
#include "ui_icon_runtime.h"
#include "ui_nav.h"
#include "ui_gesture.h"      /* 统一手势识别 */
#include "expression.h"      /* 小云格显示当前表情云图（2026-11-03） */

/****************************************************************************
 * 布局常量（390x450，16pt 栅格感）
 ****************************************************************************/

#define GRID_COLS       3
#define GRID_ROWS       2
#define PER_PAGE        (GRID_COLS * GRID_ROWS)   /* 6 */

#define CARD_W          104
#define CARD_H          82
#define CARD_RADIUS     23
#define CARD_GAP_X      14                        /* 3*104 + 2*14 = 340 */
#define GRID_X0         ((UI_SCREEN_W - (GRID_COLS * CARD_W + (GRID_COLS - 1) * CARD_GAP_X)) / 2)
/* 行距必须 >= 卡高 + 标签高 = 82 + 28 = 110，否则第一行标签会压到第二行卡片上
 * （初版写 96，静态复查时算出来重叠 16px，这里改 112）。 */
#define GRID_Y0         146
#define CARD_GAP_Y      112
#define LABEL_OFF_Y     84
#define LABEL_H         28

/****************************************************************************
 * 私有数据
 ****************************************************************************/

typedef struct
{
    const char *icon;       /* ui_icon() 的名字，NULL 表示用 glyph */
    const char *glyph;      /* 没有图标时的替代字符 */
    const char *label;      /* 卡片下的文字标签 */
    uint32_t card_bg;       /* 浅色卡片底（深色文字压在上面） */
    uint32_t tint;          /* 图标染色 */
    int id;                 /* applist_app_t */
} app_entry_t;

/* 顺序即显示顺序：第 1 页 6 个，第 2 页 4 个。
 *
 * 第 1 页放"演示时要第一眼找到"的：小云(AI) 打头，然后运动/计时器/心率/
 * 血氧/休息。第 2 页是低频设置类。
 * ★ 音乐入口**仍然隐藏**（2026-11-01 决定）：本地音乐长播放会偶发卡死
 *   触发看门狗复位，驱动重写后再开放。见 docs_ble/42、50 号文。 */
static const app_entry_t s_apps[] =
{
  /* ── 第 1 页 ── */
  { NULL,          "AI", "小云",   0xe1f2f8, 0x0a6e93, APPLIST_APP_AI      },
  { "app_sport",   NULL, "运动",   0xe8f5ee, 0x1e7a46, APPLIST_APP_SPORT   },
  { "app_timer",   NULL, "计时器", 0xfff3e3, 0x9c6000, APPLIST_APP_TIMER   },
  { "app_heart",   NULL, "心率",   0xfff0f2, 0xba3452, APPLIST_APP_HEART   },
  { "app_spo2",    NULL, "血氧",   0xe1f2f8, 0x0a6e93, APPLIST_APP_SPO2    },
  { "app_sleep",   NULL, "休息",   0xe8f0f5, 0x53697e, APPLIST_APP_SLEEP   },

  /* ── 第 2 页 ── */
  { "app_settings", NULL, "设置",  0xe8f0f5, 0x435a70, APPLIST_APP_SETTINGS },
  { NULL,          "i",  "关于",   0xe8f0f5, 0x435a70, APPLIST_APP_ABOUT   },
  { NULL,          "A",  "闹钟",   0xfff3e3, 0x9c6000, APPLIST_APP_ALARM   },
  { "weather",     NULL, "天气",   0xe1f2f8, 0x0a6e93, APPLIST_APP_WEATHER },
  { NULL,          "E",  "表情",   0xe1f2f8, 0x0a6e93, APPLIST_APP_EXPRESSION },
};

#define APP_COUNT   ((int)(sizeof(s_apps) / sizeof(s_apps[0])))
#define PAGE_COUNT  ((APP_COUNT + PER_PAGE - 1) / PER_PAGE)

static lv_obj_t *s_page;
static lv_obj_t *s_grid;        /* 卡片容器（翻页时清空重建） */
static lv_obj_t *s_footer;      /* "第 N 页 · ..." */
static lv_timer_t *s_render_timer; /* 翻页重建定时器（必须随页面一起删） */
static int       s_start;       /* 当前页第一个 app 的下标 */
static applist_open_cb_t s_open_cb;

/****************************************************************************
 * 事件
 ****************************************************************************/

static void on_card_clicked(lv_event_t *e)
{
    int id = (int)(intptr_t)lv_event_get_user_data(e);

    if (s_open_cb != NULL)
      {
        s_open_cb((applist_app_t)id);
      }
}

/* 手势：上滑下一页，下滑上一页（在第 1 页则回表盘）。
 *
 * 判定阈值统一在 ui_gesture.c（位移 45px 或速度 350px/s，无上限），
 * 这里只处理方向。 */
static void on_applist_swipe(ui_gesture_dir_t direction)
{
    if (direction == UI_GESTURE_SWIPE_UP)
      {
        if (s_start + PER_PAGE < APP_COUNT)
          {
            s_start += PER_PAGE;
            /* 不能在这里重建 —— 手势回调在 PRESSING/RELEASED 期间，
             * 当场删对象会让事件目标失效。交给下一个 timer tick 重建。 */
            lv_obj_invalidate(s_page);
          }
      }
    else if (direction == UI_GESTURE_SWIPE_DOWN)
      {
        if (s_start > 0)
          {
            s_start -= PER_PAGE;
            lv_obj_invalidate(s_page);
          }
        else
          {
            ui_nav_goto(UI_PAGE_WATCHFACE);
          }
      }
}

/****************************************************************************
 * 网格渲染
 ****************************************************************************/

static void build_grid(void)
{
    int i;

    lv_obj_clean(s_grid);

    for (i = 0; i < PER_PAGE; i++)
      {
        int idx = s_start + i;
        int col;
        int row;
        int x;
        int y;
        const app_entry_t *a;
        const expression_t *ai;
        lv_obj_t *card;
        lv_obj_t *label;

        if (idx >= APP_COUNT)
          {
            break;
          }

        a   = &s_apps[idx];
        col = i % GRID_COLS;
        row = i / GRID_COLS;
        x   = GRID_X0 + col * (CARD_W + CARD_GAP_X);
        y   = GRID_Y0 + row * CARD_GAP_Y;

        /* 卡片：浅色底 + 圆角，按下时压深一档给反馈 */

        card = lv_obj_create(s_grid);
        lv_obj_set_pos(card, x, y);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_style_bg_color(card, lv_color_hex(a->card_bg), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, CARD_RADIUS, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(card, lv_color_hex(UI_SURFACE_HI),
                                  LV_STATE_PRESSED);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        /* 翻页后重建的卡片必须自己带 EVENT_BUBBLE：ui_gesture_attach 只在
         * create 时递归开过一次，重建出来的新对象不在那次递归里，否则
         * 在第 2 页卡片上下滑返回会失效（2026-11-03 审查发现）。 */
        lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)a->id);

        /* 图标、表情云图或字母。白线图标用 image_recolor 染成该应用的
         * 语义色，染出来的颜色在浅底上对比度都 >= 3:1（图形足够，见
         * ui_theme.c）；表情图本身是彩色的，不能染色。 */

        ai = (a->id == APPLIST_APP_AI) ? expression_get(expression_current())
                                       : NULL;

        if (ai != NULL && ai->img != NULL)
          {
            /* 小云格：用当前选中的表情云图（8 款资产之一），不再显示
             * 裸的 "AI" 字母。120px 长边 * scale(160/256) ≈ 75px，
             * 正好放进 104x82 的卡片（2026-11-03）。 */
            lv_obj_t *img = lv_image_create(card);

            lv_image_set_src(img, ai->img);
            lv_image_set_scale(img, 160);
            lv_obj_center(img);
          }
        else if (a->icon != NULL)
          {
            lv_obj_t *img = lv_image_create(card);

            lv_image_set_src(img, ui_icon(a->icon));
            lv_obj_set_style_image_recolor(img, lv_color_hex(a->tint), 0);
            lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
            lv_obj_center(img);
          }
        else
          {
            lv_obj_t *g = lv_label_create(card);

            lv_label_set_text(g, a->glyph);
            lv_obj_set_style_text_font(g, UI_FONT_TITLE, 0);
            lv_obj_set_style_text_color(g, lv_color_hex(a->tint), 0);
            lv_obj_center(g);
          }

        /* 卡片下的文字标签（唯一的一处，不再像设计稿那样重复两遍） */

        label = lv_label_create(s_grid);
        lv_label_set_text(label, a->label);
        lv_obj_set_pos(label, x, y + LABEL_OFF_Y);
        lv_obj_set_size(label, CARD_W, LABEL_H);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(label, UI_FONT_CAPTION, 0);
        lv_obj_set_style_text_color(label, lv_color_hex(UI_TEXT_PRIMARY), 0);
      }

    /* 页脚提示 */

    if (s_footer != NULL)
      {
        char buf[40];

        if (PAGE_COUNT == 1)
          {
            snprintf(buf, sizeof(buf), "下滑返回表盘");
          }
        else if (s_start == 0)
          {
            snprintf(buf, sizeof(buf), "第 1 / %d 页 · 上滑看更多",
                     PAGE_COUNT);
          }
        else if (s_start + PER_PAGE >= APP_COUNT)
          {
            snprintf(buf, sizeof(buf), "第 %d / %d 页 · 下滑返回",
                     s_start / PER_PAGE + 1, PAGE_COUNT);
          }
        else
          {
            snprintf(buf, sizeof(buf), "第 %d / %d 页",
                     s_start / PER_PAGE + 1, PAGE_COUNT);
          }

        lv_label_set_text(s_footer, buf);
      }
}

/* 翻页需要延迟到下一拍：手势回调里不能删对象。
 * 用 20ms 一次的自毁定时器，检查 s_start 与「已渲染页」是否一致。 */
static int s_rendered_start = -1;

static void render_timer_cb(lv_timer_t *t)
{
    if (s_rendered_start != s_start)
      {
        s_rendered_start = s_start;
        build_grid();
      }

    (void)t;
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

lv_obj_t *applist_page_create(void)
{
    lv_obj_t *title;
    lv_obj_t *sub;

    s_start = 0;
    s_rendered_start = 0;

    s_page = lv_obj_create(lv_scr_act());
    ui_page_init(s_page);

    /* 标题：按设计稿 "所有应用" + 一行说明。
     * 与设计稿的差异：去掉"返回表盘"文字 + "返回"按钮（设计稿两者都有，
     * 语义重复；本仓统一用下滑返回，且应用入口没有顶栏）。 */

    title = lv_label_create(s_page);
    lv_label_set_text(title, "所有应用");
    lv_obj_set_pos(title, 2, 26);
    /* "所"不在 24px 子集里（烘焙时只收了当时 UI 用到的字），
     * 24px 会显示空白；18px 是全字集，标题改用 UI_FONT_SMALL（2026-11-03）。 */
    lv_obj_set_style_text_font(title, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_PRIMARY), 0);

    sub = lv_label_create(s_page);
    lv_label_set_text(sub, "点击打开 · 上滑查看更多");
    lv_obj_set_pos(sub, 2, 62);
    lv_obj_set_style_text_font(sub, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(UI_TEXT_TERTIARY), 0);

    /* 卡片容器：绝对坐标系铺满页面（取消 padding 偏移）。
     * 用绝对坐标而不是 flex：2x3 的列宽/行距已经算好，flex 反而要多写
     * 三层嵌套容器。 */

    s_grid = lv_obj_create(s_page);
    lv_obj_set_size(s_grid, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(s_grid, -UI_PAD_PAGE, -UI_PAD_PAGE);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_OFF);

    /* 页脚 */

    s_footer = lv_label_create(s_page);
    lv_label_set_text(s_footer, "");
    /* 2026-11-03：402 -> 386，页脚上移 16px，离卡片标签更近、底部留白
     * 更均衡；同时从三级色提到二级色，低对比灰字在浅底上太淡。 */
    /* 注意：子对象坐标相对内容区（页面有 20px padding），绝对 y 会 +20。
     * 原来 386 实际落在 406..434，离屏底只有 16px；按底部安全区 40 重算：
     * content_y = 450 - 40(安全区) - 28(高) - 20(padding) = 362 -> 绝对 382..410。 */
    lv_obj_set_pos(s_footer, 0, UI_SCREEN_H - UI_SAFE_BOTTOM - 28 - UI_PAD_PAGE);
    lv_obj_set_size(s_footer, UI_SCREEN_W - 2 * UI_PAD_PAGE, 28);
    lv_obj_set_style_text_align(s_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_footer, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(UI_TEXT_SECONDARY), 0);

    build_grid();

    /* 翻页渲染定时器：20ms 轮询，只在页号变化时重建。
     * ★ 必须存句柄并在 applist_page_delete() 里删掉 —— 本仓历史教训：
     *   页面定时器不删，切页后仍按 20ms 访问已释放的 s_grid，
     *   就是 use-after-free（见 ui_nav.c 文件头、timer_page.c:262）。 */
    s_render_timer = lv_timer_create(render_timer_cb, 20, NULL);

    /* 下滑返回表盘（页面级）。ui_gesture 会递归给子对象打开冒泡，
     * 所以从卡片上开始滑也能收到。 */

    ui_gesture_attach(s_page, on_applist_swipe);

    printf("[AppList] grid created (%d apps, %d page(s))\n",
           APP_COUNT, PAGE_COUNT);
    return s_page;
}

void applist_page_set_open_cb(applist_open_cb_t cb)
{
    s_open_cb = cb;
}

void applist_page_delete(void)
{
    /* 先删定时器再删对象，否则定时器会访问已释放的 s_grid（UAF） */
    if (s_render_timer != NULL)
      {
        lv_timer_delete(s_render_timer);
        s_render_timer = NULL;
      }

    if (s_page != NULL)
      {
        lv_obj_delete(s_page);
        s_page = NULL;
      }

    s_grid   = NULL;
    s_footer = NULL;
    s_start  = 0;
    s_rendered_start = 0;
}
