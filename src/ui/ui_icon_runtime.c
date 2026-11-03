/****************************************************************************
 * ui_icon_runtime.c - 运行时图标覆盖
 *
 * 目的：让设计师在 Windows 上换图标，**不用编译**。
 *
 * 做法：启动时按名字找 icons/<名字>.png。找到就用它，找不到就用
 *       编译进程序的默认图标（bandx 那套）。
 *
 * 为什么自己解 PNG 而不用 LVGL 的图片解码器：
 *   LVGL 解码文件要靠 FS 驱动，而 POSIX 文件接口在 Windows 上没有
 *   （就是我们编译时遇到 DT_DIR 那个问题）。这里直接用 libpng 解，
 *   解完自己拼成 RGB565A8 交给 LVGL —— 两个平台走同一条路，行为一致。
 *
 * 为什么是 RGB565A8：
 *   面板就是 RGB565。RGB565A8 = 每像素 2 字节颜色 + 1 字节透明度，
 *   和屏幕格式一致，绘制时直接拷贝，不用逐像素转换。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#ifdef UI_SIM
#include <png.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_icon_runtime.h"

/****************************************************************************
 * 内置图标（编译进程序的默认值）
 ****************************************************************************/

extern const lv_image_dsc_t ui_icon_heart;
extern const lv_image_dsc_t ui_icon_step;
extern const lv_image_dsc_t ui_icon_weather;
extern const lv_image_dsc_t ui_icon_battery;
extern const lv_image_dsc_t ui_icon_bluetooth;
extern const lv_image_dsc_t ui_icon_app_heart;
extern const lv_image_dsc_t ui_icon_app_spo2;
extern const lv_image_dsc_t ui_icon_app_sport;
extern const lv_image_dsc_t ui_icon_app_music;
extern const lv_image_dsc_t ui_icon_app_sleep;
extern const lv_image_dsc_t ui_icon_app_settings;
extern const lv_image_dsc_t ui_icon_app_timer;
extern const lv_image_dsc_t ui_icon_app_light;
extern const lv_image_dsc_t ui_icon_arrow_up;
extern const lv_image_dsc_t ui_icon_arrow_down;

typedef struct
{
    const char *name;
    const lv_image_dsc_t *baked;
} entry_t;

static const entry_t s_table[] =
{
  { "heart",        &ui_icon_heart        },
  { "step",         &ui_icon_step         },
  { "weather",      &ui_icon_weather      },
  { "battery",      &ui_icon_battery      },
  { "bluetooth",    &ui_icon_bluetooth    },
  { "app_heart",    &ui_icon_app_heart    },
  { "app_spo2",     &ui_icon_app_spo2     },
  { "app_sport",    &ui_icon_app_sport    },
  { "app_music",    &ui_icon_app_music    },
  { "app_sleep",    &ui_icon_app_sleep    },
  { "app_settings", &ui_icon_app_settings },
  { "app_timer",    &ui_icon_app_timer    },
  { "app_light",    &ui_icon_app_light    },
  { "arrow_up",     &ui_icon_arrow_up     },
  { "arrow_down",   &ui_icon_arrow_down   },
};

#define TABLE_SIZE ((int)(sizeof(s_table) / sizeof(s_table[0])))

/****************************************************************************
 * 用 libpng 解出 RGBA
 ****************************************************************************/

#ifdef UI_SIM

typedef struct
{
    unsigned char *data;
    int w;
    int h;
} rgba_t;

static int load_png_rgba(const char *path, rgba_t *out)
{
    FILE *f;
    png_structp png;
    png_infop info;
    png_bytep *rows;
    int y;

    memset(out, 0, sizeof(*out));

    f = fopen(path, "rb");

    if (f == NULL)
      {
        return -1;
      }

    png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

    if (png == NULL)
      {
        fclose(f);
        return -1;
      }

    info = png_create_info_struct(png);

    if (info == NULL || setjmp(png_jmpbuf(png)))
      {
        png_destroy_read_struct(&png, info ? &info : NULL, NULL);
        fclose(f);
        return -1;
      }

    png_init_io(png, f);
    png_read_info(png, info);

    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xff, PNG_FILLER_AFTER);

    out->w = (int)png_get_image_width(png, info);
    out->h = (int)png_get_image_height(png, info);

    if (out->w <= 0 || out->h <= 0 || out->w > 512 || out->h > 512)
      {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return -1;
      }

    png_read_update_info(png, info);

    out->data = (unsigned char *)malloc((size_t)out->w * out->h * 4);

    if (out->data == NULL)
      {
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return -1;
      }

    rows = (png_bytep *)malloc(sizeof(png_bytep) * out->h);

    if (rows == NULL)
      {
        free(out->data);
        out->data = NULL;
        png_destroy_read_struct(&png, &info, NULL);
        fclose(f);
        return -1;
      }

    for (y = 0; y < out->h; y++)
      {
        rows[y] = out->data + (size_t)y * out->w * 4;
      }

    png_read_image(png, rows);
    png_read_end(png, NULL);

    free(rows);
    png_destroy_read_struct(&png, &info, NULL);
    fclose(f);
    return 0;
}

/****************************************************************************
 * RGBA -> LVGL RGB565A8
 ****************************************************************************/

static unsigned char *rgba_to_565a8(const rgba_t *img)
{
    size_t px = (size_t)img->w * img->h;
    unsigned char *buf = (unsigned char *)malloc(px * 3);
    unsigned char *cb;
    unsigned char *ab;
    size_t i;

    if (buf == NULL)
      {
        return NULL;
      }

    cb = buf;
    ab = buf + px * 2;

    for (i = 0; i < px; i++)
      {
        unsigned int r = img->data[i * 4 + 0];
        unsigned int g = img->data[i * 4 + 1];
        unsigned int b = img->data[i * 4 + 2];
        unsigned int a = img->data[i * 4 + 3];
        unsigned int v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);

        cb[i * 2 + 0] = (unsigned char)(v & 0xff);
        cb[i * 2 + 1] = (unsigned char)(v >> 8);
        ab[i] = (unsigned char)a;
      }

    return buf;
}

#endif /* UI_SIM —— PNG 解码只在预览器里编 */

/****************************************************************************
 * 缓存
 ****************************************************************************/

#define MAX_OVERRIDE 20

typedef struct
{
    char name[32];
    lv_image_dsc_t dsc;
    unsigned char *buf;
} override_t;

static override_t s_over[MAX_OVERRIDE];
static int s_over_count;

#ifdef UI_SIM

static lv_image_dsc_t *try_load(const char *name)
{
    char path[128];
    rgba_t img;
    unsigned char *buf;
    lv_image_dsc_t *dsc;

    snprintf(path, sizeof(path), "icons/%s.png", name);

    if (load_png_rgba(path, &img) != 0)
      {
        return NULL;
      }

    buf = rgba_to_565a8(&img);

    if (buf == NULL)
      {
        free(img.data);
        return NULL;
      }

    dsc = &s_over[s_over_count].dsc;
    memset(dsc, 0, sizeof(*dsc));

    dsc->header.magic   = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf      = LV_COLOR_FORMAT_RGB565A8;
    dsc->header.w       = img.w;
    dsc->header.h       = img.h;
    dsc->header.stride  = img.w * 2;
    dsc->data_size      = (uint32_t)((size_t)img.w * img.h * 3);
    dsc->data           = buf;

    strncpy(s_over[s_over_count].name, name, sizeof(s_over[0].name) - 1);
    s_over[s_over_count].buf = buf;
    s_over_count++;

    free(img.data);

    printf("[Icons] 覆盖 %s <- %s (%dx%d)\n", name, path, dsc->header.w,
           dsc->header.h);
    return dsc;
}

#endif /* UI_SIM */

/* 设计师导入的图标（由 tools/import_design.py 生成）。
 * 没导入时这两个符号不存在，用弱引用兜底。 */
#include "assets/ui_user_icons.h"

/* 强引用：we always 编一个 ui_user_icons.c（没导入时是空表）。
 * 用 weak 的话，--gc-sections 会把只被 weak 引用的表当垃圾丢掉。 */
extern const ui_user_icon_t ui_user_icon_table[];
extern const unsigned int ui_user_icon_count;

const lv_image_dsc_t *ui_icon(const char *name)
{
    int i;

    /* 先查设计师导入的图标 */

    if (ui_user_icon_count > 0)
      {
        for (i = 0; i < (int)ui_user_icon_count; i++)
          {
            if (strcmp(ui_user_icon_table[i].name, name) == 0)
              {
                return ui_user_icon_table[i].dsc;
              }
          }
      }

    if (name == NULL)
      {
        return NULL;
      }

    /* 已经加载过就直接用 */

    for (i = 0; i < s_over_count; i++)
      {
        if (strcmp(s_over[i].name, name) == 0)
          {
            return &s_over[i].dsc;
          }
      }

    /* 第一次遇到：先看 icons/ 里有没有同名 PNG */

#ifdef UI_SIM
    if (s_over_count < MAX_OVERRIDE)
      {
        lv_image_dsc_t *d = try_load(name);

        if (d != NULL)
          {
            return d;
          }
      }
#endif

    /* 没有就用内置的 */

    for (i = 0; i < TABLE_SIZE; i++)
      {
        if (strcmp(s_table[i].name, name) == 0)
          {
            return s_table[i].baked;
          }
      }

    return NULL;
}

int ui_icon_is_baked(const lv_image_dsc_t *dsc)
{
    int i;

    if (dsc == NULL)
      {
        return 0;
      }

    for (i = 0; i < TABLE_SIZE; i++)
      {
        if (s_table[i].baked == dsc)
          {
            return 1;
          }
      }

    return 0;
}

void ui_icon_clear_cache(void)
{
    int i;

    for (i = 0; i < s_over_count; i++)
      {
        free(s_over[i].buf);
      }

    s_over_count = 0;
}

void ui_icon_report(void)
{
    printf("[Icons] 共 %d 个内置图标，可放到 icons/<名字>.png 覆盖\n",
           TABLE_SIZE);
    printf("[Icons] 可用名字：");
    for (int i = 0; i < TABLE_SIZE; i++)
      {
        printf("%s ", s_table[i].name);
      }
    printf("\n");
}
