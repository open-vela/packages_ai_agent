/****************************************************************************
 * sport_page.h - 运动页
 *
 * 显示计步器给出的真实数据：
 *   步数、步频、距离、消耗
 *
 * 所有数字都来自 health/pedometer.c（25Hz 读 /dev/mpu0 的真实 IMU），
 * 不是模拟值。IMU 不可用时明确显示"不可用"，不摆占位数字。
 *
 * Team 181 - Contest 2026
 ****************************************************************************/
#ifndef AI_AGENT_SPORT_PAGE_H
#define AI_AGENT_SPORT_PAGE_H

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

lv_obj_t *sport_page_create(void);
void      sport_page_delete(void);

#ifdef __cplusplus
}
#endif

#endif /* AI_AGENT_SPORT_PAGE_H */
