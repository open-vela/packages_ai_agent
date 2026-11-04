/****************************************************************************
 * About Page Header
 *
 * Application info / about page with a back button.
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef __ABOUT_PAGE_H
#define __ABOUT_PAGE_H

#include <lvgl/lvgl.h>

/****************************************************************************
 * Callback Types
 ****************************************************************************/

/**
 * Back button callback function type
 *
 * Called when the user presses the back button on the about page.
 */
typedef void (*about_back_callback_t)(void);

/****************************************************************************
 * Public API
 ****************************************************************************/

/**
 * Create the about page
 *
 * Creates a page with the app name, version and team info plus a back
 * button.
 *
 * @return About page container object, or NULL on failure
 */
lv_obj_t *about_page_create(void);

/**
 * Set the back button callback
 *
 * @param callback Function to call when the back button is pressed.
 *                 Set to NULL to disable.
 */
void about_page_set_back_callback(about_back_callback_t callback);

/**
 * Delete the about page and free resources
 */
void about_page_delete(void);

#endif /* __ABOUT_PAGE_H */
