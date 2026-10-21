/*
 * Copyright (C) 2026 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * standby_view.c — 待机界面（暗色数字时钟）。
 *
 * 长时间无操作后(由 lvgl_ui_channel.c 的 60s 空闲计时器触发),主界面切到这块
 * 极简的暗色数字时钟:大号 HH:MM(Montserrat 24) + 小号日期(月/日/周,
 * MiSans 16 CJK) + "待机中"提示。整屏比宠物/粒子页更暗更静,一眼可辨是"睡了"。
 * 任意真实交互(lvgl_ui_channel 的任一公共入口)都会调 standby_view_hide() 即时
 * 切回先前页面。时钟由 1s 定时器在 LVGL 线程刷新。
 *
 * 转场:背景与各主屏同为不透明 UI_PAL_BG,进入时直接换屏(瞬时不可见)再只对
 * 内容层做 opacity 淡入(脏区=内容包围盒,非整屏),退出时直接切回。这样淡入
 * 不再受整帧 SPI flush 带宽限制,顺滑且无硬件风险。
 *
 * 字体:HH:MM 用内置 Montserrat 24(纯 ASCII 数字/冒号),日期与提示用 app 自带
 * MiSans 16 CJK(AP 上为 3755 字全量静态字体,月/日/周/一二三…六均在)。
 */

#include <nuttx/config.h>

#ifdef CONFIG_AI_AGENT_STANDBY_UI

#include "ui/standby_view.h"
#include "ui/ui_palette.h"
#include "ui/lvgl_ui_channel.h"

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <syslog.h>
#include <errno.h>

#include <lvgl/lvgl.h>

#define TAG "standby_view"

/* 内容层淡入时长(ms)。背景与上一屏同为不透明 UI_PAL_BG,换屏瞬间不可见,
 * 只需对内容层(时钟/日期/提示)做 opacity 淡入:脏区 = 内容包围盒而非整屏,
 * 整帧 SPI flush 带宽不再是淡入瓶颈,故可缩短到 300ms 仍保持顺滑。 */
#define SCREEN_FADE_MS 300

LV_FONT_DECLARE(lv_font_misans_16_cjk);

static const char *const s_wday_cn[] = {
  "周日", "周一", "周二", "周三", "周四", "周五", "周六",
};

struct standby_state_s {
  bool initialized;
  bool visible;
  lv_obj_t *screen;
  lv_obj_t *content; /* 居中一列(clock/date/hint),淡入只作用于它 */
  lv_obj_t *clock;   /* HH:MM — Montserrat 24 */
  lv_obj_t *date;    /* M月D日 周X — MiSans 16 CJK */
  lv_obj_t *hint;    /* 待机中 — MiSans 16 CJK */
  lv_obj_t *prev_screen;
  lv_timer_t *timer; /* 1s clock refresh */
};

static struct standby_state_s s_sb;

static void refresh_time(void)
{
  if (!s_sb.initialized) return;

  time_t now = time(NULL);
  struct tm tm;
  localtime_r(&now, &tm);

  if (s_sb.clock)
    {
      char buf[16];
      snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
      lv_label_set_text(s_sb.clock, buf);
    }
  if (s_sb.date)
    {
      char buf[32];
      snprintf(buf, sizeof(buf), "%d月%d日 %s",
               tm.tm_mon + 1, tm.tm_mday, s_wday_cn[tm.tm_wday % 7]);
      lv_label_set_text(s_sb.date, buf);
    }
}

static void timer_cb(lv_timer_t *t)
{
  (void)t;
  if (s_sb.initialized && s_sb.visible)
    refresh_time();
}

/* ---------- screen show / hide (async, runs in LVGL thread) ---------- */

static void show_async_cb(void *data)
{
  (void)data;
  if (!s_sb.initialized || !s_sb.screen || s_sb.visible) return;
  s_sb.prev_screen = lv_screen_active();
  refresh_time();

  /* 背景与上一屏同为不透明 UI_PAL_BG,直接换屏瞬时可见性无差;只对内容层做
   * opacity 淡入(脏区 = 内容包围盒,而非整屏),淡入不再受整帧 SPI flush 限制。
   * 先手动把内容层压到全透明,避免换屏后首帧闪一下。 */
  if (s_sb.content)
    {
      lv_obj_set_style_opa(s_sb.content, LV_OPA_TRANSP, 0);
      lv_screen_load(s_sb.screen);
      lv_obj_fade_in(s_sb.content, SCREEN_FADE_MS, 0);
    }
  else
    {
      lv_screen_load(s_sb.screen);
    }

  s_sb.visible = true;
  syslog(LOG_INFO, "[%s] standby clock shown\n", TAG);
}

void standby_view_show(void)
{
  if (!s_sb.initialized) return;
  lvgl_ui_async_call(show_async_cb, NULL);
}

static void hide_async_cb(void *data)
{
  (void)data;
  if (!s_sb.initialized || !s_sb.visible) return;
  /* 唤醒应即时:直接切回上一屏(其内容一直是全不透明,无需淡入)。背景同色,
   * 瞬时切换无视觉跳变。 */
  if (s_sb.prev_screen)
    lv_screen_load(s_sb.prev_screen);
  s_sb.visible = false;
  syslog(LOG_INFO, "[%s] standby clock hidden\n", TAG);
}

void standby_view_hide(void)
{
  if (!s_sb.initialized) return;
  lvgl_ui_async_call(hide_async_cb, NULL);
}

bool standby_view_is_visible(void)
{
  return s_sb.visible;
}

/* ---------- init ---------- */

int standby_view_init(void)
{
  if (s_sb.initialized) return 0;

  memset(&s_sb, 0, sizeof(s_sb));

  s_sb.screen = lv_obj_create(NULL);
  if (!s_sb.screen) return -ENOMEM;
  lv_obj_set_style_bg_color(s_sb.screen, lv_color_hex(UI_PAL_BG), 0);
  lv_obj_set_style_bg_opa(s_sb.screen, LV_OPA_COVER, 0);

  /* 居中一列:大号 HH:MM 在上、日期在中、"待机中"提示在下。整体偏暗。 */
  lv_obj_t *col = lv_obj_create(s_sb.screen);
  if (!col) goto fail;
  lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(col, 0, 0);
  lv_obj_set_style_pad_all(col, 0, 0);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, 14, 0);
  lv_obj_center(col);

  s_sb.content = col;

  s_sb.clock = lv_label_create(col);
  if (!s_sb.clock) goto fail;
  lv_label_set_text(s_sb.clock, "--:--");
  lv_obj_set_style_text_font(s_sb.clock, &lv_font_montserrat_24, 0);
  lv_obj_set_style_text_color(s_sb.clock, lv_color_hex(UI_PAL_IDLE), 0);

  s_sb.date = lv_label_create(col);
  if (!s_sb.date) goto fail;
  lv_label_set_text(s_sb.date, "待机中");
  lv_obj_set_style_text_font(s_sb.date, &lv_font_misans_16_cjk, 0);
  lv_obj_set_style_text_color(s_sb.date, lv_color_hex(UI_PAL_MUTED), 0);

  s_sb.hint = lv_label_create(col);
  if (!s_sb.hint) goto fail;
  lv_label_set_text(s_sb.hint, "待机中");
  lv_obj_set_style_text_font(s_sb.hint, &lv_font_misans_16_cjk, 0);
  lv_obj_set_style_text_color(s_sb.hint, lv_color_hex(UI_PAL_MUTED), 0);

  s_sb.timer = lv_timer_create(timer_cb, 1000, NULL);
  if (s_sb.timer)
    lv_timer_set_repeat_count(s_sb.timer, -1);

  s_sb.initialized = true;
  syslog(LOG_INFO, "[%s] init OK (暗色数字时钟)\n", TAG);
  return 0;

fail:
  if (s_sb.screen) lv_obj_del(s_sb.screen);
  memset(&s_sb, 0, sizeof(s_sb));
  return -ENOMEM;
}

#endif /* CONFIG_AI_AGENT_STANDBY_UI */
