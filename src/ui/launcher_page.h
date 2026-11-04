/****************************************************************************
 * Launcher Page Header
 *
 * Application desktop with clickable icons for entering different app pages.
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#ifndef __LAUNCHER_PAGE_H
#define __LAUNCHER_PAGE_H

#include <lvgl/lvgl.h>
#include <stdbool.h>

/****************************************************************************
 * Page IDs
 ****************************************************************************/

typedef enum
{
    PAGE_PET = 0,      /* Pet display page */
    PAGE_SETTINGS = 1, /* Settings page */
    PAGE_ABOUT = 2     /* About page */
} launcher_page_id_t;

/****************************************************************************
 * Public API
 ****************************************************************************/

/**
 * Create the launcher desktop page
 *
 * Creates the main desktop with app icons. Call this first in ui_build().
 * The desktop page will be displayed on the active screen.
 */
void launcher_create(void);

/**
 * Get the desktop page object
 *
 * Returns the launcher desktop object so callers can move it to the
 * foreground or manage its layer.
 */
lv_obj_t *launcher_desktop_obj(void);

/**
 * Enter a specific application page
 *
 * @param page Page ID (PAGE_PET, PAGE_SETTINGS, PAGE_ABOUT)
 *
 * This will create the target page and move it to the foreground.
 * The previous page remains in the background.
 */
void launcher_enter_page(launcher_page_id_t page);

/**
 * Return to desktop from current page
 *
 * Destroys the current page and brings the desktop back to foreground.
 * This is typically called when the user presses a "back" button.
 */
void launcher_back_to_desktop(void);

/**
 * Check if currently on desktop page
 *
 * @return true if on desktop, false if on another page
 */
bool launcher_is_on_desktop(void);

#endif /* __LAUNCHER_PAGE_H */
