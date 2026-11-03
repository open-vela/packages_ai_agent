/****************************************************************************
 * ui_cmd.c - NSH builtin: 从串口驱动 UI 页面（演示 / 无触摸验证用）
 *
 * 为什么需要它：
 *   这块板没有可编程的触摸注入，也没有调试器。验证「某一页真的画出来了」
 *   过去只能靠人手指去点屏幕。串口能敲命令之后，可以在没有触摸的情况下
 *   切到任意页面，配合 lcd_flush 日志和 flush 像素探针做可重复的验证。
 *   演示时也用得上：讲解可以直接跳页，不用一路滑。
 *
 * 用法：
 *   ui                打印当前页 + 可用页列表
 *   ui goto <name>    切到指定页（见下表）
 *   ui next / ui prev 按 ui_page_t 顺序前后翻
 *   ui expr           打印当前表情
 *   ui expr <n>       切到第 n 款表情（会持久化到 /data/expression）
 *
 * ★ 线程安全：LVGL 以 LV_OS_NONE 构建，没有内部锁，任何非 render 线程
 *   直接调 lv_* / ui_nav_goto() 都会与 lv_timer_handler 竞态。这里一律经
 *   lvgl_ui_post() 投递到 render 线程执行（见 lvgl_ui_channel.h 的说明）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lvgl/lvgl.h>

#include "ui_nav.h"
#include "ui/expression.h"
#include "ui/lvgl_ui_channel.h"
#include "ui/pet_page.h"
#include "ui/pet_care.h"          /* DEMO-FW：演示触发钩子 */
#include "infra/ble_cmd_handler.h"

/* 页名 -> ui_page_t。名字短、好敲、和 NSH 里别的东西不冲突。 */
static const struct
{
  const char *name;
  ui_page_t   page;
} g_pages[] =
{
  { "watch",    UI_PAGE_WATCHFACE  },
  { "apps",     UI_PAGE_APPLIST    },
  { "heart",    UI_PAGE_HEART      },
  { "spo2",     UI_PAGE_SPO2       },
  { "music",    UI_PAGE_MUSIC      },
  { "timer",    UI_PAGE_TIMER      },
  { "sport",    UI_PAGE_SPORT      },
  { "sleep",    UI_PAGE_SLEEP      },
  { "settings", UI_PAGE_SETTINGS   },
  { "ai",       UI_PAGE_AI         },
  { "about",    UI_PAGE_ABOUT      },
  { "alarm",    UI_PAGE_ALARM      },
  { "weather",  UI_PAGE_WEATHER    },
  { "expr",     UI_PAGE_EXPRESSION },
};

#define PAGE_N ((int)(sizeof(g_pages) / sizeof(g_pages[0])))

static int name_to_page(const char *name, ui_page_t *out)
{
  int i;

  for (i = 0; i < PAGE_N; i++)
    {
      if (strcmp(g_pages[i].name, name) == 0)
        {
          *out = g_pages[i].page;
          return 0;
        }
    }

  return -1;
}

static const char *page_to_name(ui_page_t page)
{
  int i;

  for (i = 0; i < PAGE_N; i++)
    {
      if (g_pages[i].page == page)
        {
          return g_pages[i].name;
        }
    }

  return "?";
}

/* render 线程里真正的切页动作。data 里装 ui_page_t。 */
static lv_obj_t *find_active_page(void)
{
  lv_obj_t *scr = lv_screen_active();
  int i;

  for (i = lv_obj_get_child_count(scr) - 1; i >= 0; i--)
    {
      lv_obj_t *c = lv_obj_get_child(scr, i);

      if (c != NULL && !lv_obj_has_flag(c, LV_OBJ_FLAG_HIDDEN))
        {
          return c;
        }
    }

  return scr;
}

/* 只读调试：把当前页的对象树（屏幕绝对坐标）打到串口。
 * 用途：没有触摸注入/截图工具时，验证控件是否越界、是否被圆角遮住。 */
static int g_tree_lines;

/* BLE 协议自测：把回包直接打到串口，不需要手机/蓝牙中心。
 * 用法: ui ble {"cmd":"alarm_list"}  （JSON 整体加引号）
 * 异步回包（chat）也能收到；用完 ui blerestore 恢复真实 BLE 发送。 */
static void ble_test_sink(const char *json)
{
  printf("[ble-test] %s\n", json != NULL ? json : "(null)");
}

static void dump_tree(lv_obj_t *obj, int depth)
{
  lv_area_t a;
  const char *type = "obj";
  const char *text = "";
  int i;
  int n;

  if (obj == NULL || g_tree_lines >= 260)
    {
      return;
    }

  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN))
    {
      return;
    }

  lv_obj_get_coords(obj, &a);

  if (lv_obj_check_type(obj, &lv_label_class))
    {
      type = "label";
      text = lv_label_get_text(obj);
    }
  else if (lv_obj_check_type(obj, &lv_image_class))
    {
      type = "image";
    }
  else if (lv_obj_check_type(obj, &lv_button_class))
    {
      type = "button";
    }

  g_tree_lines++;
  printf("[ui-tree] %*s%-6s (%4d,%4d) %3dx%-3d %.*s\n", depth * 2, "",
         type, (int)a.x1, (int)a.y1,
         (int)(a.x2 - a.x1 + 1), (int)(a.y2 - a.y1 + 1),
         28, text != NULL ? text : "");

  n = (int)lv_obj_get_child_count(obj);
  for (i = 0; i < n; i++)
    {
      dump_tree(lv_obj_get_child(obj, i), depth + 1);
    }
}

static void tree_async_cb(void *data)
{
  lv_obj_t *page;

  (void)data;

  page = find_active_page();
  g_tree_lines = 0;
  printf("[ui-tree] begin page=%s screen=%dx%d\n", page_to_name(ui_nav_current()),
         (int)lv_obj_get_width(lv_screen_active()),
         (int)lv_obj_get_height(lv_screen_active()));
  if (page != NULL)
    {
      printf("[ui-tree] root pad(t=%d r=%d b=%d l=%d) content=%dx%d\n",
             (int)lv_obj_get_style_pad_top(page, LV_PART_MAIN),
             (int)lv_obj_get_style_pad_right(page, LV_PART_MAIN),
             (int)lv_obj_get_style_pad_bottom(page, LV_PART_MAIN),
             (int)lv_obj_get_style_pad_left(page, LV_PART_MAIN),
             (int)lv_obj_get_content_width(page),
             (int)lv_obj_get_content_height(page));
    }
  dump_tree(page, 0);
  printf("[ui-tree] end lines=%d\n", g_tree_lines);
}

static void hist_async_cb(void *data)
{
  (void)data;
  pet_page_open_history();
}

static void goto_async_cb(void *data)
{
  ui_page_t page = (ui_page_t)(intptr_t)data;

  ui_nav_goto(page);
}

int ui_main(int argc, char *argv[])
{
  if (argc >= 2 && strcmp(argv[1], "goto") == 0)
    {
      ui_page_t page;

      if (argc < 3)
        {
          printf("ui: usage: ui goto <name>\n");
          return 1;
        }

      if (name_to_page(argv[2], &page) != 0)
        {
          printf("ui: unknown page '%s'\n", argv[2]);
          return 1;
        }

      if (lvgl_ui_post(goto_async_cb, (void *)(intptr_t)page) != 0)
        {
          printf("ui: post failed (UI thread not running?)\n");
          return 1;
        }

      printf("ui: goto %s queued\n", argv[2]);
      return 0;
    }

  if (argc >= 2 && strcmp(argv[1], "hist") == 0)
    {
      if (lvgl_ui_post(hist_async_cb, NULL) != 0)
        {
          printf("ui: post failed (UI thread not running?)\n");
          return 1;
        }

      printf("ui: hist queued\n");
      return 0;
    }

  if (argc >= 2 && strcmp(argv[1], "ble") == 0)
    {
      if (argc < 3)
        {
          printf("ui: usage: ui ble <json>\n");
          return 1;
        }

      ble_cmd_handler_set_test_sink(ble_test_sink);
      printf("ui: ble test sink ON, dispatch: %s\n", argv[2]);
      ble_cmd_handler_recv((const uint8_t *)argv[2], (uint16_t)strlen(argv[2]), NULL);
      return 0;
    }

  if (argc >= 2 && strcmp(argv[1], "blerestore") == 0)
    {
      ble_cmd_handler_set_test_sink(NULL);
      printf("ui: ble test sink OFF\n");
      return 0;
    }

  if (argc >= 2 && strcmp(argv[1], "tree") == 0)
    {
      if (lvgl_ui_post(tree_async_cb, NULL) != 0)
        {
          printf("ui: post failed (UI thread not running?)\n");
          return 1;
        }

      printf("ui: tree queued\n");
      return 0;
    }

  if (argc >= 2 && (strcmp(argv[1], "next") == 0 ||
                    strcmp(argv[1], "prev") == 0))
    {
      int delta = (argv[1][0] == 'n') ? 1 : -1;
      ui_page_t cur = ui_nav_current();
      ui_page_t target;

      /* 线性翻页：只读 ui_nav_current()，跨线程读一个 enum 是安全的 */
      target = (ui_page_t)(((int)cur + delta + (int)PAGE_N) % (int)PAGE_N);

      if (lvgl_ui_post(goto_async_cb, (void *)(intptr_t)target) != 0)
        {
          printf("ui: post failed\n");
          return 1;
        }

      printf("ui: %s queued\n", page_to_name(target));
      return 0;
    }

  if (argc >= 2 && strcmp(argv[1], "expr") == 0)
    {
      if (argc < 3)
        {
          int cur = expression_current();

          printf("ui: expression %d/%d (%s)\n", cur, expression_count(),
                 expression_get(cur)->label);
          return 0;
        }

      {
        int index = atoi(argv[2]);

        if (index < 0 || index >= expression_count())
          {
            printf("ui: expression index out of range 0..%d\n",
                   expression_count() - 1);
            return 1;
          }

        /* expression_set 自带互斥 + 只在变化时写盘，可以在任意线程调 */
        if (expression_set(index) != 0)
          {
            printf("ui: set expression failed\n");
            return 1;
          }

        printf("ui: expression -> %d (%s)\n", index,
               expression_get(index)->label);
        return 0;
      }
    }

  /* ── DEMO-FW / DEMO-UI（2026-11-03）：演示触发命令族 ──────────────
   *   ui demo hr_alert     心率过高告警：告警气泡 + 历史 + 缓存语音
   *   ui demo greet        主动打招呼：L1 气泡 + 缓存语音
   *   ui demo alarm <s>    s 秒后一次性 cron voice 闹钟（到点播报）
   *   ui demo water        喝水提醒（DEMO-UI）
   *   ui demo sleep        该睡觉了（DEMO-UI）
   *   ui demo weather      天气变化提醒（DEMO-UI）
   *   ui demo sedentary    久坐提醒演示（DEMO-UI）
   *   ui demo milestone    步数里程碑庆祝（真实步数，只弹气泡）
   * 全部复用现有 pet_care / cron / demo_voice_cache API；只多这一小段解析，
   * 正式构建也安全（没人敲就不会发生任何事）。 */
  if (argc >= 2 && strcmp(argv[1], "demo") == 0)
    {
      const char *action = (argc >= 3) ? argv[2] : "";

      if (strcmp(action, "hr_alert") == 0)
        {
          pet_care_demo_hr_alert(155);
          printf("ui: demo hr_alert queued (155bpm -> 告警气泡 + 关怀回复)\n");
          return 0;
        }

      if (strcmp(action, "greet") == 0)
        {
          pet_care_demo_greet();
          printf("ui: demo greet queued (气泡 + 关怀回复)\n");
          return 0;
        }

      if (strcmp(action, "alarm") == 0)
        {
          int seconds = (argc >= 4) ? atoi(argv[3]) : 10;

          if (seconds <= 0)
            {
              seconds = 10;
            }

          if (pet_care_demo_alarm_after(seconds) != 0)
            {
              printf("ui: demo alarm add failed (cron full?)\n");
              return 1;
            }

          printf("ui: demo alarm in %ds queued (cron voice 闹钟)\n", seconds);
          return 0;
        }

      if (strcmp(action, "water") == 0)
        {
          pet_care_demo_water_reminder();
          printf("ui: demo water queued (喝水提醒：气泡+历史+缓存语音)\n");
          return 0;
        }

      if (strcmp(action, "sleep") == 0)
        {
          pet_care_demo_sleep_reminder();
          printf("ui: demo sleep queued (该睡觉了：气泡+历史+缓存语音)\n");
          return 0;
        }

      if (strcmp(action, "weather") == 0)
        {
          pet_care_demo_weather_change();
          printf("ui: demo weather queued (天气变化提醒：气泡+历史+缓存语音)\n");
          return 0;
        }

      if (strcmp(action, "sedentary") == 0)
        {
          pet_care_demo_sedentary();
          printf("ui: demo sedentary queued (久坐提醒：气泡+历史+缓存语音)\n");
          return 0;
        }

      if (strcmp(action, "milestone") == 0)
        {
          /* DEMO-PHONE：先切小云页，再由下一次 1Hz tick 弹真实步数气泡 */
          pet_care_demo_milestone();
          printf("ui: demo milestone queued (切小云页 + 真实步数里程碑气泡)\n");
          return 0;
        }

      printf("ui: usage: ui demo hr_alert|greet|water|sleep|weather|"
             "sedentary|milestone | ui demo alarm <seconds>\n");
      return 1;
    }

  /* 无参数：打印当前页与可用页名 */
  printf("ui: current page = %s (%d)\n", page_to_name(ui_nav_current()),
         (int)ui_nav_current());
  printf("ui: pages =");
  {
    int i;

    for (i = 0; i < PAGE_N; i++)
      {
        printf(" %s", g_pages[i].name);
      }
  }

  printf("\nui: usage: ui goto <name> | ui next | ui prev | ui expr [n] | ui tree | ui hist | ui ble <json> | ui blerestore | ui demo <hr_alert|greet|water|sleep|weather|sedentary|milestone|alarm <s>>\n");
  return 0;
}
