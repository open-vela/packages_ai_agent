/****************************************************************************
 * ui_engine.c - 数据驱动界面引擎（实现）
 *
 * 设计取舍：
 *   1. 用 cJSON 解析。工程里本来就有（音乐页在用），不新增依赖。
 *   2. 颜色/字号用**token 名**而不是裸值，这样 theme.ini 依然有效：
 *        "color": "text_primary"   读 g_theme.text_primary
 *        "color": "#ff0000"        直接写死也行
 *   3. 绑定用 "$名字"，引擎每秒刷一次，避免每帧都改文字（会狂重绘）。
 *   4. 动作只发字符串给宿主，引擎自己不认识页面 —— 这样真机和预览器
 *      可以用同一份 JSON，各自实现跳转。
 *
 * 内存：JSON 解析完立刻转成 LVGL 对象，然后整棵树释放。
 *       运行时只留 LVGL 对象，不额外占内存。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>        /* ui_engine_refresh_clock: 读 RTC */

#include "cJSON.h"
#include "ui_engine.h"
#include "ui_theme.h"
#include "ui_icon_runtime.h"
#include "ui_text_runtime.h"
#include "bubble_grid.h"

/****************************************************************************
 * 状态
 ****************************************************************************/

static cJSON *s_root;             /* 整个 ui.json */
static lv_obj_t *s_page;          /* 当前页面根对象 */
static char s_cur[32];
static ui_action_cb_t s_action_cb;

/* 需要每秒刷新的文字：{对象, 绑定表达式} */

#define MAX_BIND 48

typedef struct
{
    lv_obj_t *obj;
    char expr[24];
} bind_t;

static bind_t s_binds[MAX_BIND];
static int s_bind_count;

/* 数据源（由宿主填） */

typedef struct
{
    int   hour, minute;
    int   month, day, wday;
    int   battery;
    long  steps;
    int   bpm;
    int   spo2;
} data_t;

static data_t s_data;


void ui_engine_set_data(const ui_engine_data_t *d)
{
    if (d != NULL)
      {
        s_data.hour    = d->hour;
        s_data.minute  = d->minute;
        s_data.wday    = d->wday;
        s_data.month   = d->month;
        s_data.day     = d->day;
        s_data.battery = d->battery;
        s_data.steps   = d->steps;
        s_data.bpm     = d->bpm;
        s_data.spo2    = d->spo2;
      }
}

/* 从 RTC 读时间。这是让 $time / $date 动起来的最小必要条件 ——
 * 没有它 s_data 永远停在零值，界面就永远是一张静态图。 */
void ui_engine_refresh_clock(void)
{
    time_t now;
    struct tm tmv;

    time(&now);
    localtime_r(&now, &tmv);

    s_data.hour   = tmv.tm_hour;
    s_data.minute = tmv.tm_min;
    s_data.wday   = tmv.tm_wday;
    s_data.month  = tmv.tm_mon + 1;
    s_data.day    = tmv.tm_mday;
}

/****************************************************************************
 * 工具
 ****************************************************************************/

static const char *jstr(cJSON *o, const char *k, const char *def)
{
    cJSON *v = cJSON_GetObjectItem(o, k);

    return (cJSON_IsString(v) && v->valuestring != NULL) ? v->valuestring : def;
}

static int jint(cJSON *o, const char *k, int def)
{
    cJSON *v = cJSON_GetObjectItem(o, k);

    return cJSON_IsNumber(v) ? v->valueint : def;
}

/* 颜色：支持 "text_primary" 这种 token，也支持 "#ff0000" / "0xff0000" */

static uint32_t color_of(const char *name, uint32_t def)
{
    if (name == NULL)
      {
        return def;
      }

    if      (strcmp(name, "text_primary")   == 0) return UI_TEXT_PRIMARY;
    else if (strcmp(name, "text_secondary") == 0) return UI_TEXT_SECONDARY;
    else if (strcmp(name, "text_tertiary")  == 0) return UI_TEXT_TERTIARY;
    else if (strcmp(name, "accent")         == 0) return UI_ACCENT;
    else if (strcmp(name, "heart")          == 0) return UI_COLOR_HEART;
    else if (strcmp(name, "step")           == 0) return UI_COLOR_STEP;
    else if (strcmp(name, "weather")        == 0) return UI_COLOR_WEATHER;
    else if (strcmp(name, "surface")        == 0) return UI_SURFACE;
    else if (strcmp(name, "divider")        == 0) return UI_DIVIDER;
    else if (strcmp(name, "transparent")    == 0) return 0;
    else if (name[0] == '#') return (uint32_t)strtoul(name + 1, NULL, 16);
    else if (name[0] == '0') return (uint32_t)strtoul(name, NULL, 16);

    return def;
}

/* 字号 token -> 字体 */

static const lv_font_t *font_of(const char *name)
{
    if (name == NULL) return UI_FONT_BODY;

    if (strcmp(name, "time")    == 0) return UI_FONT_TIME;
    if (strcmp(name, "title")   == 0) return UI_FONT_TITLE;
    if (strcmp(name, "body")    == 0) return UI_FONT_BODY;
    if (strcmp(name, "caption") == 0) return UI_FONT_CAPTION;
    if (strcmp(name, "small")   == 0) return UI_FONT_SMALL;

    return UI_FONT_BODY;
}

/****************************************************************************
 * 动态数据
 ****************************************************************************/

static const char *binding_value(const char *expr)
{
    static char buf[32];

    if (strcmp(expr, "time") == 0)
      {
        snprintf(buf, sizeof(buf), "%02d:%02d", s_data.hour, s_data.minute);
        return buf;
      }

    if (strcmp(expr, "date") == 0)
      {
        static const char *const wd[7] =
          { "周日", "周一", "周二", "周三", "周四", "周五", "周六" };

        snprintf(buf, sizeof(buf), "%s  %d月%d日", wd[s_data.wday % 7],
                 s_data.month, s_data.day);
        return buf;
      }

    if (strcmp(expr, "battery") == 0)
      {
        if (s_data.battery < 0) return "--";

        snprintf(buf, sizeof(buf), "%d%%", s_data.battery);
        return buf;
      }

    if (strcmp(expr, "steps") == 0)
      {
        if (s_data.steps < 0) return "--";

        snprintf(buf, sizeof(buf), "%ld", s_data.steps);
        return buf;
      }

    if (strcmp(expr, "bpm") == 0)
      {
        if (s_data.bpm <= 0) return "--";

        snprintf(buf, sizeof(buf), "%d", s_data.bpm);
        return buf;
      }

    if (strcmp(expr, "spo2") == 0)
      {
        if (s_data.spo2 <= 0) return "--";

        snprintf(buf, sizeof(buf), "%d", s_data.spo2);
        return buf;
      }

    return NULL;
}

/* 文字：以 $ 开头就是绑定，以 @ 开头就是文案 key */

static void set_text(lv_obj_t *obj, const char *text)
{
    if (text == NULL)
      {
        return;
      }

    if (text[0] == '$')
      {
        const char *v = binding_value(text + 1);

        lv_label_set_text(obj, (v != NULL) ? v : "--");

        if (s_bind_count < MAX_BIND)
          {
            s_binds[s_bind_count].obj = obj;
            strncpy(s_binds[s_bind_count].expr, text + 1,
                    sizeof(s_binds[0].expr) - 1);
            s_bind_count++;
          }
      }
    else if (text[0] == '@')
      {
        lv_label_set_text(obj, ui_str(text + 1, text + 1));
      }
    else
      {
        lv_label_set_text(obj, text);
      }
}

/****************************************************************************
 * 动作
 ****************************************************************************/

static void on_click(lv_event_t *e)
{
    const char *act = (const char *)lv_event_get_user_data(e);

    if (act != NULL && s_action_cb != NULL)
      {
        s_action_cb(act);
      }
}

static void attach_action(lv_obj_t *obj, cJSON *node)
{
    const char *act = jstr(node, "action", NULL);

    if (act == NULL)
      {
        return;
      }

    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(obj, on_click, LV_EVENT_CLICKED, (void *)act);
}

/****************************************************************************
 * 布局
 ****************************************************************************/

static void apply_layout(lv_obj_t *obj, cJSON *node)
{
    const char *lay = jstr(node, "layout", NULL);

    if (lay == NULL)
      {
        return;
      }

    lv_obj_set_flex_flow(obj, (strcmp(lay, "row") == 0)
                             ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);

    {
        const char *jc = jstr(node, "justify", "start");
        lv_flex_align_t main_align = LV_FLEX_ALIGN_START;

        if (strcmp(jc, "center") == 0)        main_align = LV_FLEX_ALIGN_CENTER;
        else if (strcmp(jc, "end") == 0)      main_align = LV_FLEX_ALIGN_END;
        else if (strcmp(jc, "between") == 0)  main_align = LV_FLEX_ALIGN_SPACE_BETWEEN;
        else if (strcmp(jc, "around") == 0)   main_align = LV_FLEX_ALIGN_SPACE_AROUND;
        else if (strcmp(jc, "evenly") == 0)   main_align = LV_FLEX_ALIGN_SPACE_EVENLY;

        {
            const char *ac = jstr(node, "align", "center");
            lv_flex_align_t cross = LV_FLEX_ALIGN_CENTER;

            if (strcmp(ac, "start") == 0)  cross = LV_FLEX_ALIGN_START;
            else if (strcmp(ac, "end") == 0) cross = LV_FLEX_ALIGN_END;

            lv_obj_set_flex_align(obj, main_align, cross, cross);
        }
    }

    lv_obj_set_style_pad_row(obj, jint(node, "gap", UI_GAP_S), 0);
    lv_obj_set_style_pad_column(obj, jint(node, "gap", UI_GAP_S), 0);
}

static void apply_box_style(lv_obj_t *obj, cJSON *node)
{
    const char *bg = jstr(node, "bg", "transparent");
    int radius = jint(node, "radius", 0);
    int pad = jint(node, "pad", -1);

    if (strcmp(bg, "transparent") == 0)
      {
        lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
      }
    else
      {
        lv_obj_set_style_bg_color(obj, lv_color_hex(color_of(bg, 0xffffff)), 0);
        lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
      }

    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, radius, 0);

    if (pad >= 0)
      {
        lv_obj_set_style_pad_all(obj, pad, 0);
      }
    else
      {
        lv_obj_set_style_pad_all(obj, 0, 0);
      }

    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(obj, LV_SCROLLBAR_MODE_OFF);
}

static void apply_size(lv_obj_t *obj, cJSON *node)
{
    cJSON *w = cJSON_GetObjectItem(node, "w");
    cJSON *h = cJSON_GetObjectItem(node, "h");

    int width = LV_SIZE_CONTENT;
    int height = LV_SIZE_CONTENT;

    if (cJSON_IsNumber(w)) width = w->valueint;
    else if (cJSON_IsString(w) && strchr(w->valuestring, '%') != NULL)
      width = lv_pct(atoi(w->valuestring));

    if (cJSON_IsNumber(h)) height = h->valueint;
    else if (cJSON_IsString(h) && strchr(h->valuestring, '%') != NULL)
      height = lv_pct(atoi(h->valuestring));

    lv_obj_set_size(obj, width, height);

    if (jint(node, "grow", 0) != 0)
      {
        lv_obj_set_flex_grow(obj, jint(node, "grow", 0));
      }
}

/****************************************************************************
 * 各种节点
 ****************************************************************************/

static lv_obj_t *build_node(cJSON *node, lv_obj_t *parent);

static lv_obj_t *make_label(lv_obj_t *parent, cJSON *node)
{
    lv_obj_t *lbl = lv_label_create(parent);

    set_text(lbl, jstr(node, "text", ""));
    lv_obj_set_style_text_font(lbl, font_of(jstr(node, "font", "body")), 0);
    lv_obj_set_style_text_color(lbl,
                                lv_color_hex(color_of(jstr(node, "color",
                                                            "text_primary"),
                                                      0xffffff)), 0);

    if (jint(node, "wrap", 0) != 0)
      {
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
      }

    if (jint(node, "hidden", 0) != 0)
      {
        lv_obj_add_flag(lbl, LV_OBJ_FLAG_HIDDEN);
      }

    return lbl;
}

static lv_obj_t *make_icon(lv_obj_t *parent, cJSON *node)
{
    const char *name = jstr(node, "name", NULL);
    lv_obj_t *img = lv_image_create(parent);
    const lv_image_dsc_t *dsc = ui_icon(name);

    if (dsc != NULL)
      {
        lv_image_set_src(img, dsc);

        if (ui_icon_is_baked(dsc))
          {
            lv_obj_set_style_image_recolor(img,
                                           lv_color_hex(color_of(jstr(node,
                                                                      "color",
                                                                      "text_primary"),
                                                                 0xffffff)), 0);
            lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
          }
      }

    {
        int scale = jint(node, "scale", 100);

        if (scale != 100)
          {
            lv_image_set_scale(img, (uint32_t)(255 * scale / 100));
          }
    }

    return img;
}

static lv_obj_t *make_chip(lv_obj_t *parent, cJSON *node)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_t *img;
    lv_obj_t *lbl;
    const char *icon_name = jstr(node, "icon", NULL);
    const lv_image_dsc_t *dsc = icon_name ? ui_icon(icon_name) : NULL;

    apply_box_style(box, node);
    lv_obj_set_size(box, jint(node, "w", 104), jint(node, "h", 60));
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, 8, 0);

    img = lv_image_create(box);

    if (dsc != NULL)
      {
        lv_image_set_src(img, dsc);

        if (ui_icon_is_baked(dsc))
          {
            lv_obj_set_style_image_recolor(img,
                                           lv_color_hex(color_of(jstr(node,
                                                                      "tint",
                                                                      "accent"),
                                                                0xffffff)), 0);
            lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
          }
      }

    lbl = lv_label_create(box);
    set_text(lbl, jstr(node, "text", "--"));
    lv_obj_set_style_text_font(lbl, font_of(jstr(node, "font", "body")), 0);
    lv_obj_set_style_text_color(lbl,
                                lv_color_hex(color_of(jstr(node, "color",
                                                            "text_primary"),
                                                      0xffffff)), 0);

    attach_action(box, node);
    return box;
}

static lv_obj_t *make_button(lv_obj_t *parent, cJSON *node)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_t *lbl;

    apply_box_style(btn, node);

    if (strcmp(jstr(node, "bg", ""), "") == 0)
      {
        lv_obj_set_style_bg_color(btn, lv_color_hex(color_of("accent", 0x0a84ff)), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
      }

    lv_obj_set_style_radius(btn, jint(node, "radius", UI_RADIUS_PILL), 0);
    lv_obj_set_size(btn, jint(node, "w", 240), jint(node, "h", 56));
    lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lbl = lv_label_create(btn);
    set_text(lbl, jstr(node, "text", "OK"));
    lv_obj_set_style_text_font(lbl, font_of(jstr(node, "font", "body")), 0);
    lv_obj_set_style_text_color(lbl,
                                lv_color_hex(color_of(jstr(node, "color",
                                                            "surface"),
                                                      0xffffff)), 0);

    attach_action(btn, node);
    return btn;
}

static lv_obj_t *make_statusbar(lv_obj_t *parent, cJSON *node)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_t *left;
    lv_obj_t *right;
    lv_obj_t *lbl;
    lv_obj_t *img;

    apply_box_style(bar, node);
    lv_obj_set_size(bar, lv_pct(100), jint(node, "h", UI_STATUSBAR_H));
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_SPACE_BETWEEN,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    left = lv_obj_create(bar);
    apply_box_style(left, node);
    lv_obj_set_size(left, LV_SIZE_CONTENT, lv_pct(100));

    right = lv_obj_create(bar);
    apply_box_style(right, node);
    lv_obj_set_size(right, jint(node, "right_w", 96), lv_pct(100));
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(right, 6, 0);

    lbl = lv_label_create(right);
    set_text(lbl, jstr(node, "text", "$battery"));
    lv_obj_set_style_text_font(lbl, UI_FONT_CAPTION, 0);
    lv_obj_set_style_text_color(lbl,
                                lv_color_hex(color_of(jstr(node, "color",
                                                            "text_secondary"),
                                                      0xffffff)), 0);

    img = lv_image_create(right);

    {
        const lv_image_dsc_t *d = ui_icon(jstr(node, "icon", "battery"));

        if (d != NULL)
          {
            lv_image_set_src(img, d);

            if (ui_icon_is_baked(d))
              {
                lv_obj_set_style_image_recolor(img,
                                               lv_color_hex(color_of(jstr(node,
                                                                          "color",
                                                                          "text_secondary"),
                                                                    0xffffff)), 0);
                lv_obj_set_style_image_recolor_opa(img, LV_OPA_COVER, 0);
              }
          }
      }

    return bar;
}

static lv_obj_t *make_divider(lv_obj_t *parent, cJSON *node)
{
    lv_obj_t *line = lv_obj_create(parent);

    apply_box_style(line, node);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_style_bg_color(line,
                              lv_color_hex(color_of(jstr(node, "color",
                                                          "divider"),
                                                    0x9fb0cc)), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    return line;
}

/* 蜂窝里的动作：下标 -> 字符串 */
static char s_bubble_actions[16][48];

static void bubble_action_cb(int id)
{
    if (id >= 0 && id < 16 && s_action_cb != NULL && s_bubble_actions[id][0] != '\0')
      {
        s_action_cb(s_bubble_actions[id]);
      }
}

static lv_obj_t *make_bubblegrid(lv_obj_t *parent, cJSON *node)
{
    static bubble_item_t items[16];
    int count = 0;
    cJSON *apps = cJSON_GetObjectItem(node, "apps");
    cJSON *it;
    lv_obj_t *area = lv_obj_create(parent);

    apply_box_style(area, node);
    apply_size(area, node);

    if (!cJSON_IsArray(apps))
      {
        return area;
      }

    cJSON_ArrayForEach(it, apps)
      {
        if (count >= 16) break;

        items[count].icon   = ui_icon(jstr(it, "icon", NULL));
        items[count].label  = NULL;
        items[count].glyph  = jstr(it, "glyph", NULL);
        items[count].tint   = color_of(jstr(it, "color", "accent"), 0x2f6bff);
        items[count].id     = count;

        strncpy(s_bubble_actions[count], jstr(it, "action", ""),
                sizeof(s_bubble_actions[0]) - 1);
        count++;
      }

    bubble_grid_create(area, items, count, bubble_action_cb);

    return area;
}

static lv_obj_t *build_node(cJSON *node, lv_obj_t *parent)
{
    const char *type = jstr(node, "type", "box");
    lv_obj_t *obj;

    if (strcmp(type, "label") == 0)      return make_label(parent, node);
    if (strcmp(type, "icon") == 0)       return make_icon(parent, node);
    if (strcmp(type, "chip") == 0)       return make_chip(parent, node);
    if (strcmp(type, "button") == 0)     return make_button(parent, node);
    if (strcmp(type, "statusbar") == 0)  return make_statusbar(parent, node);
    if (strcmp(type, "divider") == 0)    return make_divider(parent, node);
    if (strcmp(type, "bubblegrid") == 0) return make_bubblegrid(parent, node);

    if (strcmp(type, "spacer") == 0)
      {
        obj = lv_obj_create(parent);
        apply_box_style(obj, node);
        lv_obj_set_size(obj, 0, 0);
        lv_obj_set_flex_grow(obj, 1);
        return obj;
      }

    /* box（默认） */

    obj = lv_obj_create(parent);
    apply_box_style(obj, node);
    apply_size(obj, node);
    apply_layout(obj, node);
    attach_action(obj, node);

    {
        cJSON *children = cJSON_GetObjectItem(node, "children");
        cJSON *ch;

        if (cJSON_IsArray(children))
          {
            cJSON_ArrayForEach(ch, children)
              {
                build_node(ch, obj);
              }
          }
    }

    return obj;
}

/****************************************************************************
 * 对外接口
 ****************************************************************************/

int ui_engine_load(const char *path)
{
    FILE *f;
    long len;
    char *buf;
    int ok = -1;

    if (s_root != NULL)
      {
        cJSON_Delete(s_root);
        s_root = NULL;
      }

    f = fopen(path, "rb");

    if (f == NULL)
      {
        printf("[Engine] 找不到 %s\n", path);
        return -1;
      }

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);

    buf = (char *)malloc((size_t)len + 1);

    if (buf != NULL && fread(buf, 1, (size_t)len, f) == (size_t)len)
      {
        buf[len] = '\0';

        s_root = cJSON_Parse(buf);

        if (s_root != NULL)
          {
            printf("[Engine] 已加载 %s（%ld 字节）\n", path, len);
            ok = 0;
          }
        else
          {
            printf("[Engine] JSON 解析失败：%s\n", cJSON_GetErrorPtr());
          }
      }

    free(buf);
    fclose(f);
    return ok;
}

int ui_engine_load_string(const char *json)
{
    if (s_root != NULL)
      {
        cJSON_Delete(s_root);
        s_root = NULL;
      }

    if (json == NULL || json[0] == '\0')
      {
        return -1;
      }

    s_root = cJSON_Parse(json);

    if (s_root == NULL)
      {
        printf("[Engine] 内嵌 JSON 解析失败\n");
        return -1;
      }

    printf("[Engine] 已加载内嵌界面描述\n");
    return 0;
}

/* 页面里有哪些名字可用（给宿主做校验） */
int ui_engine_has_page(const char *name)
{
    cJSON *pages;
    cJSON *page;

    if (s_root == NULL || name == NULL)
      {
        return 0;
      }

    pages = cJSON_GetObjectItem(s_root, "pages");
    page = pages ? cJSON_GetObjectItem(pages, name) : NULL;

    return page != NULL;
}

lv_obj_t *ui_engine_build_page(const char *name)
{
    cJSON *pages;
    cJSON *page;
    cJSON *children;
    cJSON *ch;
    lv_obj_t *root;

    if (s_root == NULL)
      {
        return NULL;
      }

    pages = cJSON_GetObjectItem(s_root, "pages");
    page = pages ? cJSON_GetObjectItem(pages, name) : NULL;

    if (page == NULL)
      {
        printf("[Engine] 没有页面 %s\n", name);
        return NULL;
      }

    ui_engine_destroy();
    s_bind_count = 0;

    root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(root, UI_SCREEN_W, UI_SCREEN_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, jint(page, "pad", UI_PAD_PAGE), 0);
    ui_page_apply_bg(root);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(root, LV_SCROLLBAR_MODE_OFF);

    apply_layout(root, page);

    children = cJSON_GetObjectItem(page, "children");

    if (cJSON_IsArray(children))
      {
        cJSON_ArrayForEach(ch, children)
          {
            build_node(ch, root);
          }
      }

    s_page = root;

    strncpy(s_cur, name, sizeof(s_cur) - 1);
    s_cur[sizeof(s_cur) - 1] = '\0';

    return root;
}

void ui_engine_destroy(void)
{
    /* 蜂窝网格有自己的定时器，必须先停掉再删页面 */
    bubble_grid_delete();

    if (s_page != NULL)
      {
        lv_obj_delete(s_page);
        s_page = NULL;
      }

    s_cur[0] = '\0';
    s_bind_count = 0;
}

const char *ui_engine_current(void)
{
    return s_cur;
}

void ui_engine_set_action_cb(ui_action_cb_t cb)
{
    s_action_cb = cb;
}

void ui_engine_tick(void)
{
    int i;

    for (i = 0; i < s_bind_count; i++)
      {
        const char *v = binding_value(s_binds[i].expr);

        if (v == NULL) continue;

        if (strcmp(lv_label_get_text(s_binds[i].obj), v) != 0)
          {
            lv_label_set_text(s_binds[i].obj, v);
          }
      }
}
