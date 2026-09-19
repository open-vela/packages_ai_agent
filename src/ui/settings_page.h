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

#endif /* __SETTINGS_PAGE_H */
