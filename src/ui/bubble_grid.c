/****************************************************************************
 * bubble_grid.c - Apple Watch 风格的蜂窝气泡网格
 *
 * 实现说明见 bubble_grid.h 顶部。这里只补充几个关键取舍：
 *
 * 1. 为什么用一个统一定时器推进所有动画
 *    按压、拖动、回弹、缩放全在互相影响。用 LVGL 内置动画就得给每个对象
 *    分别挂动画，互相同步和中断非常麻烦（官方教程也是这个理由）。
 *    这里每 16ms 推进一次：物理积分 → 几何求解 → 写回对象属性。
 *
 * 2. 为什么不做定点数
 *    ElenixOS 用 fx 定点数是为了在没有 FPU 的 MCU 上跑。SF32LB52 是
 *    Cortex-M33 **带 FPU**，用 float 更快也更易读。
 *
 * 3. 为什么"先快速裁剪再算距离"
 *    屏幕上大部分气泡在拖动后可能跑出视野，先丢掉它们再算三角函数/开方，
 *    能省下大量计算（官方教程明确强调这个顺序）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bubble_grid.h"
#include "ui_theme.h"
#include "ui_icon_runtime.h"

/****************************************************************************
 * 参数（按 390x450 方屏 + 8 个应用重算）
 ****************************************************************************/

#define BUBBLE_BASE      88.0f   /* 中心处气泡直径 */
#define ROW_PITCH_X      96.0f  /* 同列水平间距 */
#define ROW_PITCH_Y      104.0f   /* 行间距 */
#define X_RADIUS         130.0f  /* 满尺寸区的半宽 */
#define Y_RADIUS         130.0f  /* 满尺寸区的半高 */
#define FRINGE           120.0f  /* 从满尺寸过渡到最小尺寸的带宽 */
#define MIN_SCALE        0.74f   /* 边缘最小缩放 */

#define PRESS_SCALE      0.90f   /* 按下时缩到 90% */
#define PRESS_PULL       6.0f    /* 按下时邻居往按压点靠的像素 */
#define PRESS_PULL_R     120.0f  /* 牵引影响半径 */
#define TAP_TOLERANCE    12.0f   /* 点/拖判定阈值 */

/* ★ 「拿起一个气泡」的效果参数（2026-10-22 改）
 *
 * 原来的做法：拖动时给**所有**气泡加上同一个 (s_off_x, s_off_y)，
 * 整块气泡像拖一张纸一样一起平移 —— 就是"不像单个应用在动、
 * 更像一整页在动"的直接原因。而按下时的邻居牵引只有 6px，感知不到。
 *
 * 现在：手指按下的那个气泡 1:1 跟手并放大，邻居按距离衰减被推开。 */
#define DRAG_SCALE       1.15f   /* 被拿起的那个放大到 115% */
#define PUSH_AMOUNT      22.0f   /* 邻居最多被推开 22px（原来只有 6px） */
#define PUSH_R           160.0f  /* 推开的影响半径（原来 120px） */
#define DRAG_FOLLOW      1.0f    /* 被拿起的气泡 1:1 跟手 */
#define SPRING_K         0.18f   /* 回弹刚度 */
#define SPRING_DAMP      0.72f   /* 阻尼 */
#define MAX_OFFSET       140.0f  /* 最大拖出距离（原来 70，手指一甩就到底）*/

#define TICK_MS          16

/* 网格可视区（整屏） */
#define BUBBLE_VIEW_W    UI_SCREEN_W
#define BUBBLE_VIEW_H    UI_SCREEN_H

/* 把颜色按比例提亮（用于渐变高光） */
static uint32_t lighten(uint32_t c, int amount)
{
    int r = (c >> 16) & 0xff;
    int g = (c >> 8) & 0xff;
    int bl = c & 0xff;

    r += (255 - r) * amount / 100;
    g += (255 - g) * amount / 100;
    bl += (255 - bl) * amount / 100;

    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

/****************************************************************************
 * 数据
 ****************************************************************************/

typedef struct
{
    lv_obj_t *circle;      /* 圆底 */
    lv_obj_t *img;         /* 白色图标 */
    lv_obj_t *label;       /* 标签 */

    float base_x;          /* 布局基准坐标（相对于网格中心） */
    float base_y;

    int col;
    int row;
    int id;

    float scale;           /* 当前缩放（含按压） */
} node_t;

static lv_obj_t *s_grid;
static lv_timer_t *s_timer;
static node_t s_nodes[16];
static int s_count;

static bubble_click_cb_t s_cb;

static float s_off_x, s_off_y;      /* 被拿起气泡的位移（原来是"全局偏移"）*/
static int   s_drag_index = -1;     /* 正在被拖动的气泡下标，-1 表示没有 */
static float s_vel_x, s_vel_y;      /* 速度 */
static int s_press_index = -1;      /* 当前被按下的气泡 */
static float s_press_anim;          /* 按压动画进度 0..1 */
static float s_press_x, s_press_y;  /* 按压点（网格坐标） */

static lv_point_t s_press_pt;       /* 按下的屏幕坐标 */
static bool s_dragging;
static bool s_pressed;

/****************************************************************************
 * 布局：索引 -> 行列
 *
 * 每行容量按 3/4 交替（官方教程的蜂窝错列）。
 * 8 个应用时是 3 + 4 + 1 —— 最后一行只有一个，很难看。
 * 所以这里改成"首尾行少、中间行多"的对称模式：2 / 4 / 2。
 ****************************************************************************/

/* 每行容量。首尾行少、中间行多，蜂窝才对称。
 *
 * ★ 2026-10-25: 从 {2,4,2}（8 个）扩到 {3,4,3}（10 个），
 *   因为新增了「闹钟」和「天气」两个应用。
 *   注意：index_to_row_col() 是顺序填充，容量总和必须 >= 实际应用数，
 *   否则多出来的气泡会 fallback 到 (row0,col0)，叠在第一个气泡上。
 *   这个数组挪到文件作用域，让 row_capacity() 和 total_rows() 共用一份。 */

/* ★ 2026-10-25: 再加「AI 对话」，11 个 -> {3,4,4}。
 * 不选 {3,5,3}: 5 个/行时行宽 = 4*ROW_PITCH_X + BUBBLE_BASE = 472px >
 * 屏宽 390px，会溢出屏幕；{3,4,4} 最宽 376px，放得下。 */
static const int s_caps[] = { 3, 4, 4 };

static int row_capacity(int row)
{
    int rows = (int)(sizeof(s_caps) / sizeof(s_caps[0]));

    if (row < 0) return 0;
    if (row >= rows) return 0;

    return s_caps[row];
}

static int total_rows(void)
{
    return (int)(sizeof(s_caps) / sizeof(s_caps[0]));
}

/* 8 个应用按 2/4/2 分配正好铺满 */
static void index_to_row_col(int index, int *out_row, int *out_col)
{
    int row = 0;
    int remain = index;

    while (row < total_rows())
      {
        int cap = row_capacity(row);

        if (remain < cap)
          {
            *out_row = row;
            *out_col = remain;
            return;
          }

        remain -= cap;
        row++;
      }

    *out_row = 0;
    *out_col = 0;
}

/****************************************************************************
 * 距离场：缩放与边界收缩
 ****************************************************************************/

/* 返回 0（中心满尺寸）..1（边界）之间的归一化距离 */
static float norm_distance(float x, float y)
{
    float ax = fabsf(x) / X_RADIUS;
    float ay = fabsf(y) / Y_RADIUS;
    float d = ax > ay ? ax : ay;

    return d;
}

static float scale_for_distance(float d)
{
    /* 中心区（d <= 1 且扣掉 fringe 相当于 d <= (R-fringe)/R）满尺寸，
     * 往外线性缩到 MIN_SCALE。这里直接用归一化距离做线性过渡，够用且平滑。 */

    /* 缩放区间收窄：
     * 我们的图标是线条图标（bandx 那套），缩到 60% 时笔画只剩 1.3px，
     * 又细又虚。苹果的图标是实心图形，缩到很小仍然可读。
     * 所以这里最小只缩到 74%，保证笔画仍然立得住。 */
    float t = (d - 0.45f) / (1.0f - 0.45f);

    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    return 1.0f - (1.0f - MIN_SCALE) * t;
}

/****************************************************************************
 * 每帧刷新
 ****************************************************************************/

static void layout_update(void)
{
    int i;
    /* 用常量而不是 lv_obj_get_width()：网格创建时父容器的布局还没算，
     * 查尺寸会拿到 0，整簇会堆到左上角。网格固定是整屏大小。 */
    float cx = (float)BUBBLE_VIEW_W / 2.0f;
    float cy = (float)BUBBLE_VIEW_H / 2.0f;


    for (i = 0; i < s_count; i++)
      {
        node_t *n = &s_nodes[i];
        float x, y, d, sc, r;
        int size, px, py;

        /* 基准坐标 */
        x = n->base_x;
        y = n->base_y;

        /* ★ 被拿起的那个气泡：1:1 跟手 + 放大。
         * 只动它一个 —— 这正是"单个应用在动"和"整页在动"的区别。 */

        if (i == s_drag_index)
          {
            x += s_off_x;
            y += s_off_y;
          }

        /* 距离场缩放 */

        d = norm_distance(x, y);
        sc = scale_for_distance(d);

        /* 被拿起的放大，压下的缩小 */

        if (i == s_drag_index)
          {
            sc *= DRAG_SCALE;
          }
        else if (s_press_anim > 0.0f && i == s_press_index)
          {
            sc *= 1.0f - (1.0f - PRESS_SCALE) * s_press_anim;
          }

        /* ★ 邻居让位。
         *
         * 原来这里是"向按压点轻微牵引"，幅度只有 6px，还只在 s_press_anim
         * 期间有效 —— 拖动时反而不生效（牵引用的是固定按压点，不是气泡
         * 跟手后的实时位置）。
         *
         * 现在改成：以**被拖动气泡的当前位置**为中心，把邻居沿径向推开，
         * 距离越近推得越多。这样拖动时能看到周围的气泡在躲开。 */

        if (s_drag_index >= 0 && i != s_drag_index)
          {
            float drag_x = s_nodes[s_drag_index].base_x + s_off_x;
            float drag_y = s_nodes[s_drag_index].base_y + s_off_y;
            float dx = x - drag_x;
            float dy = y - drag_y;
            float dist = sqrtf(dx * dx + dy * dy);

            if (dist < PUSH_R && dist > 0.5f)
              {
                float k = (1.0f - dist / PUSH_R);

                k = k * k;                 /* 平方衰减：近处推得多，远处几乎不动 */
                x += dx / dist * PUSH_AMOUNT * k;
                y += dy / dist * PUSH_AMOUNT * k;
              }
          }
        else if (s_press_anim > 0.0f && s_press_index >= 0 &&
                 i != s_press_index)
          {
            /* 还没开始拖：保留原来按下瞬间的轻微牵引，给一点"整块在呼吸"的反馈 */
            float dx = s_press_x - x;
            float dy = s_press_y - y;
            float dist = sqrtf(dx * dx + dy * dy);

            if (dist < PRESS_PULL_R && dist > 0.5f)
              {
                float k = (1.0f - dist / PRESS_PULL_R) * s_press_anim;

                x += dx / dist * PRESS_PULL * k;
                y += dy / dist * PRESS_PULL * k;
              }
          }

        n->scale = sc;

        size = (int)(BUBBLE_BASE * sc + 0.5f);
        if (size < 12) size = 12;

        /* 圆心在 (cx + x, cy + y)，LVGL 要左上角坐标 */

        px = (int)(cx + x - size / 2.0f);
        py = (int)(cy + y - size / 2.0f);

        lv_obj_set_size(n->circle, size, size);
        lv_obj_set_pos(n->circle, px, py);

        /* 图标跟着圆等比缩放：68px 图标缩到圆内 62% */

        if (n->img != NULL)
          {
            r = size * 0.58f;
            lv_image_set_scale(n->img, (uint32_t)(r * 256.0f / 68.0f));
            lv_obj_center(n->img);
          }

        /* 标签放在圆下方 */

        if (n->label != NULL)
          {
            lv_obj_set_width(n->label, size + 44);
            lv_obj_set_style_text_align(n->label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_pos(n->label, px - 22, py + size + 2);
          }
      }
}

/****************************************************************************
 * 物理：拖动跟手 + 松手回弹
 ****************************************************************************/

static void physics_step(void)
{
    if (!s_dragging)
      {
        /* 弹簧回中：F = -k*x，叠加阻尼 */

        s_vel_x += -SPRING_K * s_off_x;
        s_vel_y += -SPRING_K * s_off_y;

        s_vel_x *= SPRING_DAMP;
        s_vel_y *= SPRING_DAMP;

        s_off_x += s_vel_x;
        s_off_y += s_vel_y;

        /* 足够小就归零，避免无限抖动（也就停止了重绘） */

        if (fabsf(s_off_x) < 0.3f && fabsf(s_vel_x) < 0.3f)
          {
            s_off_x = 0.0f;
            s_vel_x = 0.0f;
          }

        if (fabsf(s_off_y) < 0.3f && fabsf(s_vel_y) < 0.3f)
          {
            s_off_y = 0.0f;
            s_vel_y = 0.0f;
          }

        /* 回中完成 → 松开"被拿起的是哪一个"，邻居让位也随之消失 */
        if (s_off_x == 0.0f && s_off_y == 0.0f)
          {
            s_drag_index = -1;
          }
      }

    /* 按压动画平滑逼近目标 */

    if (s_pressed && s_press_index >= 0)
      {
        s_press_anim += (1.0f - s_press_anim) * 0.35f;
      }
    else
      {
        s_press_anim += (0.0f - s_press_anim) * 0.30f;

        if (s_press_anim < 0.01f)
          {
            s_press_anim = 0.0f;
            s_press_index = -1;
          }
      }
}

static void tick_cb(lv_timer_t *t)
{
    float ox = s_off_x;
    float oy = s_off_y;
    float pa = s_press_anim;

    (void)t;

    physics_step();

    /* 只有真的变了才重排，静止时零开销 */

    if (fabsf(ox - s_off_x) > 0.01f || fabsf(oy - s_off_y) > 0.01f ||
        fabsf(pa - s_press_anim) > 0.01f)
      {
        layout_update();
      }
}

/****************************************************************************
 * 输入
 ****************************************************************************/

static int hit_test(int sx, int sy)
{
    int i;
    int best = -1;
    float best_scale = 0.0f;

    /* 命中时优先选 scale 更大的（视觉上更靠前，符合预期） */

    for (i = 0; i < s_count; i++)
      {
        lv_area_t a;

        if (s_nodes[i].circle == NULL) continue;

        lv_obj_get_coords(s_nodes[i].circle, &a);

        if (sx >= a.x1 && sx <= a.x2 && sy >= a.y1 && sy <= a.y2)
          {
            if (s_nodes[i].scale > best_scale)
              {
                best_scale = s_nodes[i].scale;
                best = i;
              }
          }
      }

    return best;
}

static void on_pressed(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t pt;
    int idx;

    (void)e;

    if (indev == NULL) return;

    lv_indev_get_point(indev, &pt);
    s_press_pt = pt;
    s_pressed = true;
    s_dragging = false;

    /* 上一次的回弹可能还没走完。新一次触摸直接掐断，
     * 否则两个气泡会同时受 s_off 影响。 */
    if (s_drag_index >= 0)
      {
        s_off_x = 0.0f;
        s_off_y = 0.0f;
        s_vel_x = 0.0f;
        s_vel_y = 0.0f;
        s_drag_index = -1;
      }

    idx = hit_test(pt.x, pt.y);

    if (idx >= 0)
      {
        s_press_index = idx;

        /* 记录按压点（网格坐标），供邻居牵引用 */

        s_press_x = s_nodes[idx].base_x + s_off_x;
        s_press_y = s_nodes[idx].base_y + s_off_y;
      }
    else
      {
        s_press_index = -1;
      }
}

static void on_pressing(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    lv_point_t pt;
    float dx, dy;

    (void)e;

    if (!s_pressed || indev == NULL) return;

    lv_indev_get_point(indev, &pt);

    dx = (float)(pt.x - s_press_pt.x);
    dy = (float)(pt.y - s_press_pt.y);

    if (!s_dragging &&
        (fabsf(dx) > TAP_TOLERANCE || fabsf(dy) > TAP_TOLERANCE))
      {
        s_dragging = true;
        s_drag_index = s_press_index;   /* ★ 记住"拿起了哪一个" */
        s_press_index = -1;             /* 取消放下瞬间的按压反馈 */
      }

    if (s_dragging)
      {
        /* 只移动被拿起的那个气泡（1:1 跟手）。
         * 邻居的位移由 layout_update 按距离算，不在这里统一平移。 */
        s_off_x = dx * DRAG_FOLLOW;
        s_off_y = dy * DRAG_FOLLOW;

        if (s_off_x > MAX_OFFSET) s_off_x = MAX_OFFSET;
        if (s_off_x < -MAX_OFFSET) s_off_x = -MAX_OFFSET;
        if (s_off_y > MAX_OFFSET) s_off_y = MAX_OFFSET;
        if (s_off_y < -MAX_OFFSET) s_off_y = -MAX_OFFSET;

        /* 拖出速度，松手后转成惯性 */

        s_vel_x = dx * 0.15f;
        s_vel_y = dy * 0.15f;

        layout_update();
      }
}

static void on_released(lv_event_t *e)
{
    int idx = s_press_index;

    (void)e;

    s_pressed = false;

    /* 没拖动 + 按下命中同一个 -> 算点击 */

    if (!s_dragging && idx >= 0 && s_cb != NULL)
      {
        s_cb(s_nodes[idx].id);
      }

    /* 松手：s_dragging 立刻清零（下一帧起弹簧开始回中），
     * 但 s_drag_index **保留** —— 回弹动画要靠它知道把哪个气泡往回收。
     * 等 s_off 收拢到 0，physics_step 再把它清掉。 */
    s_dragging = false;
}

/****************************************************************************
 * 创建
 ****************************************************************************/

lv_obj_t *bubble_grid_create(lv_obj_t *parent, const bubble_item_t *items,
                             int count, bubble_click_cb_t cb)
{
    int i;

    if (count > (int)(sizeof(s_nodes) / sizeof(s_nodes[0])))
      {
        count = (int)(sizeof(s_nodes) / sizeof(s_nodes[0]));
      }

    s_cb = cb;
    s_count = count;
    s_off_x = s_off_y = 0.0f;
    s_vel_x = s_vel_y = 0.0f;
    s_press_index = -1;
    s_press_anim = 0.0f;
    s_pressed = false;
    s_dragging = false;

    s_grid = lv_obj_create(parent);
    /* 按父容器尺寸算，不能写死整屏：这个网格挂在页面里一个更小的区域中，
     * 用整屏尺寸会让"中心"偏到屏幕外，整簇看起来是歪的。 */
    lv_obj_set_size(s_grid, BUBBLE_VIEW_W, BUBBLE_VIEW_H);
    lv_obj_set_pos(s_grid, 0, 0);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_CLICKABLE);

    memset(s_nodes, 0, sizeof(s_nodes));

    for (i = 0; i < count; i++)
      {
        node_t *n = &s_nodes[i];
        int row = 0, col = 0;
        int cap;
        float half_span;

        index_to_row_col(i, &row, &col);
        cap = row_capacity(row);
        half_span = (cap - 1) / 2.0f;

        n->row = row;
        n->col = col;
        n->id = items[i].id;
        n->scale = 1.0f;

        /* 每行独立居中：col 减去本行半宽 */

        n->base_x = (col - half_span) * ROW_PITCH_X;
        n->base_y = (row - (total_rows() - 1) / 2.0f) * ROW_PITCH_Y;

        /* 圆底 */

        n->circle = lv_obj_create(s_grid);
        lv_obj_set_style_radius(n->circle, LV_RADIUS_CIRCLE, 0);
        /* 竖向渐变：顶部提亮 18%，底部保持原色。
         * 纯平色块看着像色卡，一点渐变就有球体的体积感 —— 这也是
         * 苹果图标和"自己画的圆"最直观的差别。 */
        lv_obj_set_style_bg_color(n->circle, lv_color_hex(items[i].tint), 0);
        lv_obj_set_style_bg_grad_color(n->circle,
                                       lv_color_hex(lighten(items[i].tint, 30)), 0);
        lv_obj_set_style_bg_grad_dir(n->circle, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(n->circle, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(n->circle, 0, 0);
        lv_obj_set_style_pad_all(n->circle, 0, 0);
        lv_obj_clear_flag(n->circle, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_clear_flag(n->circle, LV_OBJ_FLAG_CLICKABLE);

        /* 白色图标 */

        if (items[i].icon != NULL)
          {
            n->img = lv_image_create(n->circle);
            lv_image_set_src(n->img, items[i].icon);

            /* 内置图标是白色线条画，压在彩色圆上要染白；
             * 设计师放进 icons/ 的彩色图保持原色，一染就全白了。 */
            if (ui_icon_is_baked(items[i].icon))
              {
                lv_obj_set_style_image_recolor(n->img, lv_color_white(), 0);
                lv_obj_set_style_image_recolor_opa(n->img, LV_OPA_COVER, 0);
              }
            lv_obj_center(n->img);
          }
        else if (items[i].glyph != NULL)
          {
            /* 没有图标就用一个字符顶着（"关于"用 "i"） */
            lv_obj_t *g = lv_label_create(n->circle);

            lv_label_set_text(g, items[i].glyph);
            lv_obj_set_style_text_font(g, UI_FONT_BODY, 0);
            lv_obj_set_style_text_color(g, lv_color_white(), 0);
            lv_obj_center(g);
          }

        /* 标签 */

        if (items[i].label != NULL)
          {
            n->label = lv_label_create(s_grid);
            lv_label_set_text(n->label, items[i].label);
            lv_obj_set_style_text_font(n->label, UI_FONT_CAPTION, 0);
            lv_obj_set_style_text_color(n->label,
                                        lv_color_hex(UI_TEXT_PRIMARY), 0);
          }
      }

    /* 输入：三个事件只改状态，物理在定时器里统一推进 */

    lv_obj_add_event_cb(s_grid, on_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_grid, on_pressing, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_grid, on_released, LV_EVENT_RELEASED, NULL);

    layout_update();

    /* 定时器要留着句柄：页面销毁时如果不停掉它，回调里会去操作
     * 已经释放的对象 —— 直接崩。 */
    s_timer = lv_timer_create(tick_cb, TICK_MS, NULL);

    printf("[BubbleGrid] created with %d bubbles\n", count);
    return s_grid;
}

void bubble_grid_delete(void)
{
    if (s_timer != NULL)
      {
        lv_timer_delete(s_timer);
        s_timer = NULL;
      }

    if (s_grid != NULL)
      {
        lv_obj_delete(s_grid);
        s_grid = NULL;
      }

    memset(s_nodes, 0, sizeof(s_nodes));
    s_count = 0;
}
