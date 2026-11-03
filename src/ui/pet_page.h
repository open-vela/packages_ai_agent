/****************************************************************************
 * Pet Display Page Header
 *
 * Cloud pet character with interactive chat bubble.
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef __PET_PAGE_H
#define __PET_PAGE_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

/****************************************************************************
 * Callback Types
 ****************************************************************************/

/**
 * Back button callback function type
 *
 * Called when user presses the back button on the pet page.
 */
typedef void (*pet_back_callback_t)(void);

/****************************************************************************
 * Public API
 ****************************************************************************/

/**
 * Create the pet display page
 *
 * Creates a page with:
 * - Cloud pet character (emoji placeholder, replace with actual image)
 * - Chat bubble (hidden by default)
 * - Back button
 * - Idle animations (floating + blinking)
 *
 * @return Pet page container object, or NULL on failure
 *
 * @note The caller is responsible for setting the back callback via
 *       pet_page_set_back_callback() after creation.
 */
lv_obj_t *pet_page_create(void);

/**
 * Set the back button callback
 *
 * @param callback Function to call when back button is pressed.
 *                 Set to NULL to disable.
 */
void pet_page_set_back_callback(pet_back_callback_t callback);

/**
 * Update the response text in the chat bubble
 *
 * @param text Response text to display (UTF-8)
 *
 * Shows the bubble if it was hidden.
 */
void pet_page_update_response(const char *text);

/**
 * Update the status text below the pet
 *
 * @param text Status text to display (UTF-8)
 */
void pet_page_update_status(const char *text);

/**
 * After @a delay_ms, restore the AI text layer to the time greeting.
 *
 * Used by callers that overwrite the text layer with a temporary message
 * (one-shot heart-rate measurement, P1b): the greeting comes back by itself
 * so the page does not stay stuck on a stale result. Reuses the same
 * one-shot lv_timer pattern as the cloud tap reply.
 *
 * Must be called on the LVGL thread (it creates an lv_timer). Non-LVGL
 * callers reach it through lvgl_ui_post().
 *
 * @param delay_ms Delay before the greeting is restored
 */
void pet_page_schedule_greeting_restore(uint32_t delay_ms);

/** 打开「对话记录」全屏浮层（仅 LVGL 线程；串口调试/演示用）。 */
void pet_page_open_history(void);

/**
 * Delete the pet page and free resources
 *
 * Stops animations and destroys the page container.
 */
void pet_page_delete(void);

#endif /* __PET_PAGE_H */
