/*
 * pet_display.h - Cloud mascot (pet) display module for the AI Agent UI.
 *
 * Owns the pet image, its emotion layers (question marks, sparkles,
 * blush) and the emotion state machine driving the animations.
 * The pet is created on an existing LVGL screen; the caller runs the
 * LVGL event loop (lv_timer_handler).
 */

#pragma once

#include <lvgl/lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pet emotions, driven by the conversation state machine */
typedef enum {
    PET_EMOTION_IDLE = 0,      /* floating + blinking */
    PET_EMOTION_ACTIVE,        /* click bounce */
    PET_EMOTION_LISTEN,        /* user is talking (recording) */
    PET_EMOTION_THINKING,      /* ASR/LLM pending */
    PET_EMOTION_SPEAKING,      /* TTS playback */
    PET_EMOTION_HAPPY,         /* positive reply */
    PET_EMOTION_CONFUSED,      /* question / uncertain reply */
} pet_emotion_t;

/**
 * Create the pet on the given screen, centered on it.
 * Must be called from the LVGL thread after lv_init().
 *
 * @param screen  parent screen (lv_screen_active() usually)
 * @return 0 on success, negative errno otherwise
 */
int pet_display_init(lv_obj_t *screen);

/**
 * Switch the pet to the given emotion (thread-safe via lv_async_call
 * when called from a non-LVGL thread).
 */
void pet_display_set_emotion(pet_emotion_t emotion);

/**
 * Return the current emotion.
 */
pet_emotion_t pet_display_get_emotion(void);

/**
 * Show/hide the emotion layers (question marks, sparkles, blush).
 * Used when the bubble area covers the pet.
 */
void pet_display_set_layers_visible(bool visible);

/**
 * Show or hide the whole pet stage (body + emotion layers).
 *
 * The pet lives on the same screen as the launcher desktop, which is an
 * opaque full-screen object stacked in front of it.  LVGL does not cull
 * invalidation for an object that is merely covered by an opaque sibling:
 * lv_obj_area_is_visible() checks LV_OBJ_FLAG_HIDDEN and the ancestor clip
 * chain, never siblings.  So the pet's idle float animation kept invalidating
 * its 300x300 area ~30 times a second, and with the display in full-refresh
 * mode every one of those became a whole-screen repaint of a pet nobody can
 * see.  Setting LV_OBJ_FLAG_HIDDEN makes that visibility check fail, and
 * dropping the animations removes the per-tick style writes as well.
 *
 * Must be called from the LVGL thread.
 */
void pet_display_set_stage_visible(bool visible);

/**
 * Return the pet image widget (for tap events / z-order).
 */
lv_obj_t *pet_display_get_obj(void);

#ifdef __cplusplus
}
#endif
