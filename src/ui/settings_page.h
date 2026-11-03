/****************************************************************************
 * Settings Page Header
 *
 * LLM configuration display (read-only placeholder for now).
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef __SETTINGS_PAGE_H
#define __SETTINGS_PAGE_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

/****************************************************************************
 * Callback Types
 ****************************************************************************/

/**
 * Back button callback function type
 */
typedef void (*pet_back_callback_t)(void);

/****************************************************************************
 * Public API
 ****************************************************************************/

/**
 * Create the settings page
 *
 * @return Settings page container object, or NULL on failure
 */
lv_obj_t *settings_page_create(void);

/**
 * Set the back button callback
 *
 * @param callback Function to call when back button is pressed
 */
void settings_page_set_back_callback(pet_back_callback_t callback);

/**
 * Update LLM host display
 *
 * @param host LLM host URL (e.g., "https://api.stepfun.com/v1")
 */
void settings_page_update_host(const char *host);

/**
 * Update LLM model display
 *
 * @param model LLM model name (e.g., "step-3.7-flash")
 */
void settings_page_update_model(const char *model);

/**
 * Update API key display (masked)
 *
 * @param key API key (will be masked to first 4 chars + ****)
 */
void settings_page_update_key(const char *key);

/**
 * Clean up settings page resources
 */
void settings_page_delete(void);

/**
 * 若当前处于息屏状态则点亮屏幕（物理按键的逃生口，P17）。
 * 返回 true = 本次按键被用于唤醒、调用方应吞掉它不执行原动作；
 * 返回 false = 本来就亮着，调用方正常处理。
 */
bool settings_page_wake_screen(void);

/****************************************************************************
 * 亮度档位（98 号文 FW-B；BLE settings_get / settings_set 用）
 *
 * 档位是设置页「亮度」那一行的 UI 档位索引，0..max 连续，
 * 对应 20%..100%（现有 s_bright_levels 数组，本次不改档位值）。
 * 任何线程可调：硬件亮度同步生效，页面标签经 render 线程异步刷新。
 ****************************************************************************/

/**
 * 当前亮度档位索引（0..settings_page_brightness_max()）。
 *
 * @return 档位索引；设置页从未创建过时返回默认档位（100%，即 max）
 */
int settings_page_brightness_get(void);

/**
 * 设置亮度档位并立即应用。
 *
 * 越界自动钳位到 [0, max]；复用设置页现有的 brightness_apply() 通路，
 * 页面「亮度」行文案随档位更新（跨线程安全）。
 *
 * @param level 目标档位索引，0..max
 * @return 实际生效的档位索引（钳位后的值）
 */
int settings_page_brightness_set(int level);

/**
 * 最大亮度档位索引（现有档位数组长度 - 1，当前为 4 ↔ 100%）。
 *
 * @return 最大档位索引
 */
int settings_page_brightness_max(void);

#endif /* __SETTINGS_PAGE_H */
