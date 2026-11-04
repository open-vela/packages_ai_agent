/****************************************************************************
 * ui_text_runtime.h - 运行时文案
 *
 *   ui_str("settings.brightness", "亮度")
 *
 * text.ini 里有这个 key 就用文件里的，没有就用第二个参数的默认值。
 * 设计师改 text.ini 再点「重载」，文案立刻变，不用编译。
 ****************************************************************************/

#ifndef UI_TEXT_RUNTIME_H
#define UI_TEXT_RUNTIME_H

#ifdef __cplusplus
extern "C" {
#endif

/** 读文案。key 不存在返回 fallback。 */
const char *ui_str(const char *key, const char *fallback);

/** 重新读 text.ini */
void ui_text_load(const char *path);

/** 从内存里的 ini 文本加载（真机用） */
void ui_text_load_string(const char *ini);

/** 检查这些 key 用到的汉字，字体里有没有。返回缺字数量。 */
int ui_text_check_font(void);

#ifdef __cplusplus
}
#endif

#endif
