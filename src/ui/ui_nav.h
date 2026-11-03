/****************************************************************************
 * ui_nav.h - 页面导航中枢
 *
 * 为什么要一个单独的中枢：
 *   之前页面之间互相直接调 create/delete（launcher 调 pet_page_create，
 *   pet 调 launcher_back_to_desktop），形成环，谁也不知道当前在哪一页。
 *   结果就是定时器泄漏、重复创建、返回时找不到"上一页"。
 *
 * 这里改成：所有页面切换都走 ui_nav_goto()，由它负责
 *   1. 先删旧页（含该页的定时器）
 *   2. 再建新页
 *   3. 记住当前页，供返回用
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_UI_NAV_H
#define AI_AGENT_UI_NAV_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
  UI_PAGE_WATCHFACE = 0,   /* 表盘（默认页） */
  UI_PAGE_APPLIST,         /* 应用列表 */
  UI_PAGE_HEART,           /* 心率 */
  UI_PAGE_SPO2,            /* 血氧 */
  UI_PAGE_MUSIC,           /* 音乐（语音播放的遥控器） */
  UI_PAGE_TIMER,           /* 计时器（倒计时） */
  UI_PAGE_SPORT,           /* 运动（步数/距离/卡路里） */
  UI_PAGE_SLEEP,           /* 静止/休息 */
  UI_PAGE_SETTINGS,        /* 设置 */
  UI_PAGE_AI,              /* AI 对话（桌宠页） */
  UI_PAGE_ABOUT,           /* 关于 */
  UI_PAGE_ALARM,           /* 闹钟（cron 持久化任务列表） */
  UI_PAGE_WEATHER,         /* 天气（北京，Open-Meteo） */
  UI_PAGE_EXPRESSION       /* 小云表情选择（2026-11-02 批次 3） */
} ui_page_t;

/**
 * 切到指定页面。会先销毁当前页。
 */
void ui_nav_goto(ui_page_t page);

/**
 * 返回上一页（只有一层：回到表盘）。
 */
void ui_nav_back(void);

/**
 * 当前页。
 */
ui_page_t ui_nav_current(void);

/**
 * 初始化导航，进入表盘。
 */
void ui_nav_init(void);

/** 给页面挂"下滑返回"手势。
 *
 * 之前只有表盘和应用列表挂了手势，心率/血氧/设置/关于/AI 都没有，
 * 点进去之后出不来 —— 这是漏掉的功能，不是设计取舍。
 *
 * 统一在这里实现，各页面创建完调一次即可。
 * 阈值 60px，和表盘/应用列表保持一致。 */
void ui_nav_attach_back_swipe(lv_obj_t *page);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_UI_NAV_H */
