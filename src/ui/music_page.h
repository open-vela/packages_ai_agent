/****************************************************************************
 * music_page.h - 音乐页
 *
 * 存在的理由：音乐有 8 个工具（播放/暂停/停止/状态/搜索/音量/进度/恢复），
 * 语音全都能用，但屏幕上没有任何入口 —— 用户点了应用列表的「音乐」
 * 只会看到一句 not implemented。这个页面把已有能力接到屏幕上。
 *
 * 设计取舍：页面**不自己播放音乐**（播放要 URL，那是语音搜索的活），
 * 它是语音播放的**遥控器**：显示当前在放什么、放到哪，并提供暂停/切歌/音量。
 * 没在播放时明确显示「未播放」并提示怎么说，而不是假装有内容。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_MUSIC_PAGE_H
#define AI_AGENT_MUSIC_PAGE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 创建音乐页并返回页面对象。重复调用返回同一实例。 */
lv_obj_t *music_page_create(void);

/** 销毁页面与其刷新定时器。导航切页时调用。 */
void music_page_delete(void);

/** 销毁页面与其刷新定时器。导航切页时调用。 */
void music_page_delete(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_MUSIC_PAGE_H */