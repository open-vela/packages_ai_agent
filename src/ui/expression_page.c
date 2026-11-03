/****************************************************************************
 * expression_page.c - 小云表情选择页（2x3 宫格，上滑翻页）
 *
 * 布局（390x450）：
 *
 *   ┌──────────────────────────────┐
 *   │ 小云表情                      │  24px 标题
 *   │ 选择你喜欢的小云               │  18px 副标题
 *   │ ┌────┐ ┌────┐ ┌────┐         │
 *   │ │ 😊 │ │ 😌 │ │ 😆 │         │  60px 缩略图 + 名字
 *   │ └────┘ └────┘ └────┘         │
 *   │ ┌────┐ ┌────┐ ┌────┐         │
 *   │ │ 🤩 │ │ 😂 │ │ 😍 │         │
 *   │ └────┘ └────┘ └────┘         │
 *   │      第 1 / 2 页 · 共 8 款    │
 *   └──────────────────────────────┘
 *
 * 交互：点格子 = 选中并立即返回表盘（用户目的就是"换一款"，多停一页
 *      反而多一次下滑）；上/下滑翻页；下滑到第一页再下滑回表盘。
 *
 * ★ 缩略图缩放：源图是 120px 长边的 crop 资产，格子里显示 60px。
 *   lv_image 的 lv_obj_set_size **不会**缩放图片（只裁剪），必须
 *   lv_image_set_scale(128) = 0.5x。docs_ble/91c P0-1 实测过这个坑：
 *   不设 scale 的话 120px 图会溢出格子、把相邻格盖住。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>

#include "expression_page.h"
#include "ui/expression.h"
#include "ui_theme.h"
#include "ui_nav.h"
#include "ui_gesture.h"

/****************************************************************************
 * 布局常量
 ****************************************************************************/

#define COLS          3
#define ROWS          2
#define PER_PAGE      (COLS * ROWS)   /* 6 */

#define CELL_W        104
#define CELL_H        104
#define CELL_GAP_X    14
#define CELL_GAP_Y    12
#define GRID_X0       ((UI_SCREEN_W - (COLS * CELL_W + (COLS - 1) * CELL_GAP_X)) / 2)
#define GRID_Y0       150

/* 缩略图缩放：120px 源 -> ~60px 显示，0.5x = 128 */
#define THUMB_SCALE   160

/****************************************************************************
 * 私有数据
 ****************************************************************************/

static lv_obj_t *s_page;
static lv_obj_t *s_grid;
static lv_obj_t *s_footer;
static lv_timer_t *s_render_timer;
static int s_start;            /* 当前页第一个表情的下标 */
static int s_rendered_start = -1;
static int s_pending_pick = -1; /* 待生效的选中索引（-1 = 无） */

/****************************************************************************
 * 事件
 ****************************************************************************/

static void on_cell_clicked(lv_event_t *e)
{
    /* ★ 不能在这里直接 expression_set + ui_nav_goto：那会在 CLICKED
     * 事件处理**过程中**把当前页（也就是事件目标的祖先）删掉，
     * LVGL 返回事件链时可能访问已释放对象。
     * 这里只记下"用户想选哪一款"，由 20ms 定时器在事件上下文之外落地。
     * （本仓 applist 一直是同步切的，没炸过；但那是运气，不是契约。） */
    s_pending_pick = (int)(intptr_t)lv_event_get_user_data(e);
}

static void on_page_swipe(ui_gesture_dir_t direction)
{
    int n = expression_count();

    if (direction == UI_GESTURE_SWIPE_UP)
      {
        if (s_start + PER_PAGE < n)
          {
            s_start += PER_PAGE;
          }
      }
    else if (direction == UI_GESTURE_SWIPE_DOWN)
      {
        if (s_start > 0)
          {
            s_start -= PER_PAGE;
          }
        else
          {
            ui_nav_goto(UI_PAGE_WATCHFACE);
          }
      }
}

/****************************************************************************
 * 渲染
 ****************************************************************************/

static void build_grid(void)
{
    int n = expression_count();
    int current = expression_current();
    int i;

    lv_obj_clean(s_grid);

    for (i = 0; i < PER_PAGE; i++)
      {
        int index = s_start + i;
        int col;
        int row;
        int x;
        int y;
        const expression_t *ex;
        lv_obj_t *cell;
        lv_obj_t *img;
        lv_obj_t *name;
        bool selected;

        if (index >= n)
          {
            break;
          }

        ex       = expression_get(index);
        selected = (index == current);
        col      = i % COLS;
        row      = i / COLS;
        x        = GRID_X0 + col * (CELL_W + CELL_GAP_X);
        y        = GRID_Y0 + row * (CELL_H + CELL_GAP_Y);

        /* 格子：选中态用主色描边（不靠颜色单独表达，还有名字加粗态
         * 由 footer 的"当前：xx"承担语义 —— 色盲也能分辨）。 */

        cell = lv_obj_create(s_grid);
        lv_obj_set_pos(cell, x, y);
        lv_obj_set_size(cell, CELL_W, CELL_H);
        lv_obj_set_style_bg_color(cell, lv_color_hex(UI_SURFACE), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(cell, 18, 0);
        lv_obj_set_style_pad_all(cell, 0, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(cell, lv_color_hex(UI_SURFACE_HI),
                                  LV_STATE_PRESSED);

        if (selected)
          {
            lv_obj_set_style_border_width(cell, 3, 0);
            lv_obj_set_style_border_color(cell, lv_color_hex(UI_ACCENT), 0);
          }
        else
          {
            lv_obj_set_style_border_width(cell, 0, 0);
          }

        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        /* 同 applist：翻页重建的格子要自己开冒泡，否则第 2 页下滑返回失效。 */
        lv_obj_add_flag(cell, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(cell, on_cell_clicked, LV_EVENT_CLICKED,
                            (void *)(intptr_t)index);

        img = lv_image_create(cell);
        lv_image_set_src(img, ex->img);
        /* ★ 必须 set_scale：lv_obj_set_size 对图片只裁剪不缩放 */
        lv_image_set_scale(img, THUMB_SCALE);
        lv_obj_align(img, LV_ALIGN_TOP_MID, 0, 6);

        name = lv_label_create(cell);
        lv_label_set_text(name, ex->label);
        lv_obj_set_width(name, CELL_W - 8);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(name, UI_FONT_SMALL, 0);
        lv_obj_set_style_text_color(name,
                                    lv_color_hex(selected ? UI_ACCENT
                                                          : UI_TEXT_SECONDARY),
                                    0);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -6);
      }

    if (s_footer != NULL)
      {
        char buf[64];
        int pages = (n + PER_PAGE - 1) / PER_PAGE;

        snprintf(buf, sizeof(buf), "第 %d / %d 页 · 共 %d 款 · 当前：%s",
                 s_start / PER_PAGE + 1, pages, n,
                 expression_get(current)->label);
        lv_label_set_text(s_footer, buf);
      }
}

/* 翻页与选中态都需要延迟重建：手势回调里不能删对象；
 * expression_set 之后本页就要销毁了，也不必重建。 */
static void render_timer_cb(lv_timer_t *t)
{
    (void)t;

    /* 先处理"点选"：写持久化 + 回表盘。此时不在事件上下文里，删页安全。 */
    if (s_pending_pick >= 0)
      {
        int pick = s_pending_pick;

        s_pending_pick = -1;
        expression_set(pick);
        ui_nav_goto(UI_PAGE_WATCHFACE);
        return;
      }

    if (s_rendered_start != s_start)
      {
        s_rendered_start = s_start;
        build_grid();
      }
}

/****************************************************************************
 * 公开接口
 ****************************************************************************/

lv_obj_t *expression_page_create(void)
{
    lv_obj_t *title;
    lv_obj_t *sub;
    int n = expression_count();
    int pages = (n + PER_PAGE - 1) / PER_PAGE;

    s_start = 0;
    s_rendered_start = 0;
    s_pending_pick = -1;

    s_page = lv_obj_create(lv_scr_act());
    ui_page_init(s_page);

    title = lv_label_create(s_page);
    lv_label_set_text(title, "小云表情");
    lv_obj_set_pos(title, 2, 26);
    lv_obj_set_style_text_font(title, UI_FONT_TITLE, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(UI_TEXT_PRIMARY), 0);

    sub = lv_label_create(s_page);
    lv_label_set_text(sub, "选择你喜欢的小云 · 上滑翻页");
    lv_obj_set_pos(sub, 2, 62);
    lv_obj_set_style_text_font(sub, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(sub, lv_color_hex(UI_TEXT_TERTIARY), 0);

    s_grid = lv_obj_create(s_page);
    lv_obj_set_size(s_grid, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(s_grid, -UI_PAD_PAGE, -UI_PAD_PAGE);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_OFF);

    s_footer = lv_label_create(s_page);
    lv_label_set_text(s_footer, "");
    /* 相对内容区坐标：原来 400 实际落在 420..450，底边正好贴屏幕下沿。
     * 按底部安全区 40 重算：content_y = 450 - 40 - 30 - 20 = 360 -> 380..410。 */
    lv_obj_set_pos(s_footer, 0, UI_SCREEN_H - UI_SAFE_BOTTOM - 30 - UI_PAD_PAGE);
    lv_obj_set_size(s_footer, UI_SCREEN_W - 2 * UI_PAD_PAGE, 30);
    lv_obj_set_style_text_align(s_footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_footer, UI_FONT_SMALL, 0);
    lv_obj_set_style_text_color(s_footer, lv_color_hex(UI_TEXT_TERTIARY), 0);

    build_grid();

    /* 20ms 轮询渲染（只在页号变化时重建）。
     * ★ 必须在 delete 里删掉，否则切页后访问已释放的 s_grid = UAF。 */
    s_render_timer = lv_timer_create(render_timer_cb, 20, NULL);

    /* 下滑返回表盘（页面级；ui_gesture 会递归给子对象开冒泡） */
    ui_gesture_attach(s_page, on_page_swipe);

    printf("[ExpressionPage] created (%d expressions, %d page(s))\n",
           n, pages);
    return s_page;
}

void expression_page_delete(void)
{
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
    s_rendered_start = -1;
    s_pending_pick = -1;
}
