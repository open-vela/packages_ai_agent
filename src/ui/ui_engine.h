/****************************************************************************
 * ui_engine.h - 数据驱动界面引擎
 *
 * 目的：让设计稿的**全部内容**（布局、层级、间距、颜色、文字、图标、
 *       跳转）都写在 ui.json 里，程序启动时按数据把界面搭出来。
 *
 *       设计师改 JSON → 点重载 → 立刻看到效果，完全不用编译器。
 *
 * 为什么不用"每个页面一个 .c 文件"：
 *   那样布局就写死在编译产物里了，改一个间距都要重新编译。
 *   设计师不可能为了挪 4 个像素去装一套交叉编译工具链。
 *
 * 节点类型（够描述现在所有页面）：
 *   page        页面根（背景、内边距、整体布局）
 *   box         通用容器（横排/竖排/自由，间距、对齐、圆角、底色）
 *   spacer      弹性占位（把内容顶开）
 *   label       文字
 *   icon        图标
 *   chip        图标 + 文字的横排小块（表盘的数据条）
 *   button      胶囊按钮
 *   statusbar   状态栏（电量）
 *   bubblegrid  Apple Watch 蜂窝应用网格
 *   divider     一条分隔线
 *
 * 动态数据绑定：文字里写 $time / $steps / $bpm 之类，引擎每秒刷新。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef UI_ENGINE_H
#define UI_ENGINE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 读取并解析 ui.json。返回 0 成功。 */
int ui_engine_load(const char *path);

/** 从内存里的 JSON 文本加载（真机没有文件系统时用这个）。 */
int ui_engine_load_string(const char *json);

/** 按名字建一个页面，返回根对象（失败返回 NULL）。 */
lv_obj_t *ui_engine_build_page(const char *name);

/** 销毁当前页面 */
void ui_engine_destroy(void);

/** 当前页面名 */
const char *ui_engine_current(void);

/** 页面数据源。
 *
 * ★ 重要：必须有人周期性地把真实数据填进来（见 ui_engine_refresh_now），
 *   否则 s_data 保持全零，页面上的 $time 会永远是 "00:00"、$date 是
 *   "周日 0月0日"，而且因为值不变，ui_engine_tick() 一个标签都不会更新
 *   —— 于是 LVGL 没有任何对象失效，屏幕定在首帧不动。
 *   2026-10-21 真机的"表盘卡死"就是这个：系统一切正常、心跳在涨，
 *   但整屏只 flush 过开机那 8 次。 */
typedef struct
{
  int   hour;
  int   minute;
  int   wday;
  int   month;
  int   day;
  int   battery;   /* <0 表示未知，显示 "--" */
  long  steps;     /* <0 表示未知 */
  int   bpm;       /* <=0 表示未知 */
  int   spo2;      /* <=0 表示未知 */
} ui_engine_data_t;

/** 设置页面数据源。 */
void ui_engine_set_data(const ui_engine_data_t *d);

/** 从系统 RTC 读一次时间并填充数据源（电量/步数等由调用方另填）。
 *  这是最小可用实现：保证 $time / $date 会动起来。 */
void ui_engine_refresh_clock(void);

/** 刷新动态数据（时间、步数、心率…）。调用前请先 set_data / refresh_clock。 */
void ui_engine_tick(void);

/** 页面里可用的动作，由宿主（服务器/固件）实现 */
typedef void (*ui_action_cb_t)(const char *action);

/** 注册动作回调。action 形如 "goto:heart" / "back" / "measure:heart" */
void ui_engine_set_action_cb(ui_action_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* UI_ENGINE_H */
