/****************************************************************************
 * expression.h - 小云表情（当前选中的那一款）的对外接口。
 *
 * 2026-11-02（批次 3）：第一批只做 8 款，用于打通「索引表 -> 资产 -> 表盘 /
 * AI 页显示 -> 持久化」整条链路。链路验证通过后再扩到设计稿的 50 款
 * （docs_ble/92 号方案里的「先档 1、再档 2」）。
 *
 * 与 font / icon 一样，表情图是**编译期烘焙**的 RGB565A8 常量：本构建
 * 没有开 LV_USE_FS_* 也没有 PNG 解码器，运行时只能引用 const 位图。
 *
 * ★ 索引顺序必须与 LuminaWatch 的 CloudAvatars.kt 一致。表情页的
 *   「第 N 款」、表盘显示、持久化里的数字全部依赖这个顺序；一旦按文件
 *   名字排序就整体错位（docs_ble/91c 附录-1 的坑）。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef AI_AGENT_UI_EXPRESSION_H
#define AI_AGENT_UI_EXPRESSION_H

#include <lvgl/lvgl.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ★ 前 8 个 = CloudAvatars.kt 的前 8 个，顺序逐字对齐。
 *   将来扩到 50 款时只在**尾部追加**，已存的索引不迁移。
 *   参考表: lumina watch-ui-v1/firmware-ui/lumina_expressions.c */
typedef enum
{
  EXPRESSION_SMILE = 0,       /* 微笑        wei_xiao          */
  EXPRESSION_CALM,            /* 平静        ping_jing         */
  EXPRESSION_GIGGLE,          /* 嘻嘻        xi_xi             */
  EXPRESSION_YES,             /* 欧耶        ou_ye             */
  EXPRESSION_LAUGH,           /* 哈哈哈      ha_ha_ha          */
  EXPRESSION_SORROW,          /* 哀          ai                */
  EXPRESSION_AOLIGEI,         /* 奥利给      ao_li_gei         */
  EXPRESSION_KEEP_SMILE,      /* 保持微笑    bao_chi_wei_xiao  */
  EXPRESSION_COUNT
} expression_id_t;

/** 一款表情的元数据。id 是 CloudAvatars 里的英文标识（用于排查）。 */
typedef struct
{
  const char *id;
  const char *label;          /* 中文名，显示在表情页 */
  const lv_image_dsc_t *img;  /* RGB565A8 位图，编译期常量 */
} expression_t;

/** 表情总数（当前 8）。 */
int expression_count(void);

/** 取第 index 款；越界返回第一款，永不返回 NULL。 */
const expression_t *expression_get(int index);

/** 当前选中的表情索引。首次调用会从 /data/expression 读取；
 *  文件不存在/内容非法时用 EXPRESSION_SMILE。 */
int expression_current(void);

/** 切换表情。只有索引真的变化时才写一次 /data/expression（1 字节），
 *  重复设置同一款不碰 flash。/data 不可写时静默降级为内存态。 */
int expression_set(int index);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_UI_EXPRESSION_H */
