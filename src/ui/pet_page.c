/****************************************************************************
 * Pet Display Page
 *
 * Displays the cloud pet character with interactive bubble chat.
 * Extracted from the original ai_agent_main.c display functions.
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include "pet_page.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Cloud bitmap assets (300x300 ARGB8888, shared with pet_display) */
extern const uint32_t pet_cloud_open_px[];
extern const uint32_t pet_cloud_blink_px[];

#define PET_CLOUD_SIZE 300
#define PET_CLOUD_STRIDE (PET_CLOUD_SIZE * 4)

static const lv_image_dsc_t s_cloud_open_dsc = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_ARGB8888,
                .flags = 0,
                .w = PET_CLOUD_SIZE,
                .h = PET_CLOUD_SIZE,
                .stride = PET_CLOUD_STRIDE,
                .reserved_2 = 0 },
    .data_size = PET_CLOUD_SIZE * PET_CLOUD_STRIDE,
    .data = (const uint8_t *)pet_cloud_open_px,
};

static const lv_image_dsc_t s_cloud_blink_dsc = {
    .header = { .magic = LV_IMAGE_HEADER_MAGIC,
                .cf = LV_COLOR_FORMAT_ARGB8888,
                .flags = 0,
                .w = PET_CLOUD_SIZE,
                .h = PET_CLOUD_SIZE,
                .stride = PET_CLOUD_STRIDE,
                .reserved_2 = 0 },
    .data_size = PET_CLOUD_SIZE * PET_CLOUD_STRIDE,
    .data = (const uint8_t *)pet_cloud_blink_px,
};

static lv_obj_t *pet_page_container;
static lv_obj_t *pet_image;
static lv_obj_t *bubble_label;
static lv_obj_t *status_label;
static pet_back_callback_t back_callback = NULL;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * Handle back button click
 */
static void on_back_clicked(lv_event_t *e)
{
    printf("[PetPage] Back button clicked\n");
    if (back_callback != NULL)
    {
        back_callback();
    }
}

/**
 * Handle pet image click (toggle bubble or trigger recording)
 */
static void on_pet_clicked(lv_event_t *e)
{
    printf("[PetPage] Pet image clicked\n");

    if (bubble_label != NULL)
    {
        /* Toggle bubble visibility */

        if (lv_obj_has_flag(bubble_label, LV_OBJ_FLAG_HIDDEN))
        {
            lv_obj_clear_flag(bubble_label, LV_OBJ_FLAG_HIDDEN);
            lv_label_set_text(bubble_label, "你好呀，我是小云\n有什么可以帮到你的吗？");
        }
        else
        {
            lv_obj_add_flag(bubble_label, LV_OBJ_FLAG_HIDDEN);
        }

        lv_timer_handler();
    }

    /* TODO: Trigger audio recording and VAD (phase B) */
}

/****************************************************************************
 * Pet Animation (idle floating + blink)
 ****************************************************************************/

static lv_anim_t pet_y_anim;
static lv_anim_t pet_blink_anim;
static bool pet_blink_state = false;

/**
 * Pet idle Y-axis floating animation callback
 */
static void pet_float_anim_cb(void *var, int32_t value)
{
    lv_obj_set_y((lv_obj_t *)var, value);
    lv_timer_handler();
}

/**
 * Pet blink animation callback (swap cloud open/blink bitmaps)
 */
static void pet_blink_anim_cb(void *var, int32_t value)
{
    if (value == 0)
    {
        lv_image_set_src((lv_obj_t *)var, &s_cloud_open_dsc);
    }
    else
    {
        lv_image_set_src((lv_obj_t *)var, &s_cloud_blink_dsc);
    }
    lv_timer_handler();
    pet_blink_state = !pet_blink_state;
}

/**
 * Start idle animations for the pet
 */
static void pet_start_idle_animation(void)
{
    /* Y-axis floating animation (±6px, 3s loop) */

    lv_anim_init(&pet_y_anim);
    lv_anim_set_var(&pet_y_anim, pet_image);
    lv_anim_set_values(&pet_y_anim, 0, -6);
    lv_anim_set_exec_cb(&pet_y_anim, pet_float_anim_cb);
    lv_anim_set_time(&pet_y_anim, 1500);
    lv_anim_set_repeat_count(&pet_y_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_playback_delay(&pet_y_anim, 0);
    lv_anim_start(&pet_y_anim);

    /* Blink animation (every 3s, 100ms blink) */

    lv_anim_init(&pet_blink_anim);
    lv_anim_set_var(&pet_blink_anim, pet_image);
    lv_anim_set_values(&pet_blink_anim, 0, 1);
    lv_anim_set_exec_cb(&pet_blink_anim, pet_blink_anim_cb);
    lv_anim_set_time(&pet_blink_anim, 100);
    lv_anim_set_playback_time(&pet_blink_anim, 100);
    lv_anim_set_repeat_delay(&pet_blink_anim, 2800);
    lv_anim_set_repeat_count(&pet_blink_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&pet_blink_anim);
}

/**
 * Stop idle animations
 */
static void pet_stop_animation(void)
{
    lv_anim_del(pet_image, NULL);
}

/****************************************************************************
 * Public API
 ****************************************************************************/

/**
 * Create the pet display page
 *
 * @return Pet page container object, or NULL on failure
 */
lv_obj_t *pet_page_create(void)
{
    printf("[PetPage] Creating pet display page\n");

    /* Stop previous animations if running */

    pet_stop_animation();

    /* Create pet page container */

    pet_page_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(pet_page_container, 390, 450);
    lv_obj_set_pos(pet_page_container, 0, 0);
    lv_obj_set_style_bg_color(pet_page_container, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(pet_page_container, 0, 0);
    lv_obj_set_style_pad_all(pet_page_container, 0, 0);

    /* Back button (top-left, enlarged for easy tapping) */

    lv_obj_t *back_btn = lv_btn_create(pet_page_container);
    lv_obj_set_size(back_btn, 90, 45);
    lv_obj_set_pos(back_btn, 10, 10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x4a4a6a), 0);
    lv_obj_set_style_radius(back_btn, 10, 0);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "< 返回");
    lv_obj_set_style_text_font(back_label, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xffffff), 0);
    lv_obj_center(back_label);

    lv_obj_add_event_cb(back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);

    /* Title */

    lv_obj_t *title = lv_label_create(pet_page_container);
    lv_label_set_text(title, "小云 - 你的桌面伙伴");
    lv_obj_set_style_text_font(title, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x88ccff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    /* Cloud pet image (300x300 bitmap scaled to 180px) */

    pet_image = lv_image_create(pet_page_container);
    lv_image_set_src(pet_image, &s_cloud_open_dsc);
    lv_image_set_scale(pet_image, 128); /* 0.5x -> 150px */
    lv_obj_align(pet_image, LV_ALIGN_CENTER, 0, -10);
    lv_obj_add_event_cb(pet_image, on_pet_clicked, LV_EVENT_CLICKED, NULL);

    /* Chat bubble (initially hidden) */

    bubble_label = lv_label_create(pet_page_container);
    lv_label_set_text(bubble_label, "你好呀，我是小云\n有什么可以帮到你的吗？");
    lv_obj_set_style_text_font(bubble_label, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(bubble_label, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(bubble_label, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_border_color(bubble_label, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_border_width(bubble_label, 1, 0);
    lv_obj_set_style_radius(bubble_label, 8, 0);
    lv_obj_set_style_pad_all(bubble_label, 10, 0);
    lv_obj_set_width(bubble_label, 280);
    lv_label_set_long_mode(bubble_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(bubble_label, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_add_flag(bubble_label, LV_OBJ_FLAG_HIDDEN); /* Initially hidden */

    /* Status label */

    status_label = lv_label_create(pet_page_container);
    lv_label_set_text(status_label, "点击云朵开始对话");
    lv_obj_set_style_text_font(status_label, &lv_font_simsun_16_cjk, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(0xaaaaaa), 0);
    lv_obj_align(status_label, LV_ALIGN_BOTTOM_MID, 0, -30);

    /* Start idle animation */

    pet_start_idle_animation();

    printf("[PetPage] Pet display page created successfully\n");
    return pet_page_container;
}

/**
 * Set the back button callback
 *
 * @param callback Function to call when back button is pressed
 */
void pet_page_set_back_callback(pet_back_callback_t callback)
{
    back_callback = callback;
}

/**
 * Update the pet response text in the bubble
 *
 * @param text Response text to display
 */
void pet_page_update_response(const char *text)
{
    if (bubble_label != NULL && text != NULL)
    {
        lv_label_set_text(bubble_label, text);
        lv_obj_clear_flag(bubble_label, LV_OBJ_FLAG_HIDDEN);
        lv_timer_handler();
        printf("[PetPage] Response updated: %s\n", text);
    }
}

/**
 * Update the pet status text
 *
 * @param status Status text to display
 */
void pet_page_update_status(const char *status)
{
    if (status_label != NULL && status != NULL)
    {
        lv_label_set_text(status_label, status);
        lv_timer_handler();
    }
}

/**
 * Clean up pet page resources
 */
void pet_page_delete(void)
{
    printf("[PetPage] Deleting pet page\n");
    pet_stop_animation();
    if (pet_page_container != NULL)
    {
        lv_obj_del(pet_page_container);
        pet_page_container = NULL;
    }
    back_callback = NULL;
}
