/****************************************************************************
 * ui_text_runtime.c - 运行时文案
 *
 * 文本从 text.ini 读，key = 值。找不到就用代码里的默认值。
 *
 * 注意字体覆盖：界面用的是内置点阵字体（cjk_24 只有 349 个汉字）。
 * 设计师如果写了字体里没有的字，真机上会显示成空白。
 * ui_text_check_font() 会检查并打印缺哪些字。
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ui_text_runtime.h"
#include "ui_theme.h"

#define MAX_KEYS   128
#define MAX_VAL    128

typedef struct
{
    char key[64];
    char val[MAX_VAL];
} kv_t;

static kv_t s_kv[MAX_KEYS];
static int  s_count;

static void rtrim(char *s)
{
    int n = (int)strlen(s);

    while (n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' ||
                     s[n - 1] == ' ' || s[n - 1] == '\t'))
      {
        s[--n] = '\0';
      }
}

/* 解析一段 ini 文本（文件和内嵌字符串共用） */
static void text_parse(const char *text)
{
    char line[512];
    const char *p = text;

    while (p != NULL && *p != '\0' && s_count < MAX_KEYS)
      {
        const char *nl = strchr(p, '\n');
        size_t n = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        char *eq;
        char *key;
        char *val;

        if (n >= sizeof(line)) n = sizeof(line) - 1;
        memcpy(line, p, n);
        line[n] = '\0';
        p = (nl != NULL) ? nl + 1 : NULL;

        key = line;

        rtrim(line);

        while (*key == ' ' || *key == '\t') key++;

        if (*key == '\0' || *key == '#' || *key == ';')
          {
            continue;
          }

        eq = strchr(key, '=');

        if (eq == NULL)
          {
            continue;
          }

        *eq = '\0';
        val = eq + 1;
        rtrim(key);

        while (*val == ' ' || *val == '\t') val++;

        strncpy(s_kv[s_count].key, key, sizeof(s_kv[0].key) - 1);
        strncpy(s_kv[s_count].val, val, MAX_VAL - 1);
        s_count++;
      }
}

void ui_text_load_string(const char *ini)
{
    s_count = 0;

    if (ini != NULL)
      {
        text_parse(ini);
        printf("[Text] 已加载内嵌文案（%d 条）\n", s_count);
      }
}

void ui_text_load(const char *path)
{
    FILE *f;
    long len;
    char *buf;

    s_count = 0;

    if (path == NULL)
      {
        return;
      }

    f = fopen(path, "r");

    if (f == NULL)
      {
        printf("[Text] 没有 %s，用代码里的默认文案\n", path);
        return;
      }

    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);

    buf = (char *)malloc((size_t)len + 1);

    if (buf != NULL && fread(buf, 1, (size_t)len, f) == (size_t)len)
      {
        buf[len] = '\0';
        text_parse(buf);
        printf("[Text] 已加载 %s（%d 条）\n", path, s_count);
      }

    free(buf);
    fclose(f);
}

const char *ui_str(const char *key, const char *fallback)
{
    int i;

    if (key == NULL)
      {
        return fallback;
      }

    for (i = 0; i < s_count; i++)
      {
        if (strcmp(s_kv[i].key, key) == 0)
          {
            return s_kv[i].val;
          }
      }

    return fallback;
}

int ui_text_check_font(void)
{
    int i;
    int missing = 0;

    for (i = 0; i < s_count; i++)
      {
        const char *p = s_kv[i].val;

        while (*p != '\0')
          {
            uint32_t cp = 0;
            int bytes = 0;

            /* 手写一个最小的 UTF-8 解码，不引新依赖 */

            if ((*p & 0x80) == 0)       { cp = (uint8_t)*p;         bytes = 1; }
            else if ((*p & 0xE0) == 0xC0) { cp = (uint8_t)(*p & 0x1F); bytes = 2; }
            else if ((*p & 0xF0) == 0xE0) { cp = (uint8_t)(*p & 0x0F); bytes = 3; }
            else                          { cp = (uint8_t)(*p & 0x07); bytes = 4; }

            for (int k = 1; k < bytes; k++)
              {
                cp = (cp << 6) | (uint8_t)(p[k] & 0x3F);
              }

            if (cp >= 0x20)
              {
                lv_font_glyph_dsc_t d;

                if (!lv_font_get_glyph_dsc(UI_FONT_BODY, &d, cp, 0))
                  {
                    printf("[Text] 字体缺字: U+%04X (%s)\n", (unsigned)cp,
                           s_kv[i].key);
                    missing++;
                  }
              }

            p += bytes;
          }
      }

    return missing;
}
