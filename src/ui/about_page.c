/****************************************************************************
 * About Page
 *
 * Shows application / team info with a back button.
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include "about_page.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *about_page_container;
static about_back_callback_t back_callback = NULL;

/* Sans-serif (Noto Sans SC) font family */
extern const lv_font_t ui_font_sans_16;
extern const lv_font_t ui_font_sans_16_bold;
extern const lv_font_t ui_font_sans_32;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * Handle back button click
 */
static void on_back_clicked(lv_event_t *e)
{
    printf("[About] Back button clicked\n");
    (void)e;
    if (back_callback != NULL)
    {
        back_callback();
    }
}

/****************************************************************************
 * Public API
 ****************************************************************************/

/**
 * Create the about page
 */
lv_obj_t *about_page_create(void)
{
    printf("[About] Creating about page\n");

    about_page_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(about_page_container, 390, 450);
    lv_obj_set_pos(about_page_container, 0, 0);
    lv_obj_set_style_bg_color(about_page_container, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(about_page_container, 0, 0);
    lv_obj_set_style_pad_all(about_page_container, 20, 0);

    /* Back button */
    lv_obj_t *back_btn = lv_btn_create(about_page_container);
    lv_obj_set_size(back_btn, 90, 45);
    lv_obj_set_pos(back_btn, 10, 10);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x4a4a6a), 0);
    lv_obj_set_style_radius(back_btn, 10, 0);

    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, "< 返回");
    lv_obj_set_style_text_font(back_label, &ui_font_sans_16, 0);
    lv_obj_set_style_text_color(back_label, lv_color_hex(0xffffff), 0);
    lv_obj_center(back_label);

    lv_obj_add_event_cb(back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);

    /* Title */
    lv_obj_t *title = lv_label_create(about_page_container);
    lv_label_set_text(title, "关于");
    lv_obj_set_style_text_font(title, &ui_font_sans_16_bold, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x88ccff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    /* App name */
    lv_obj_t *app = lv_label_create(about_page_container);
    lv_label_set_text(app, "HerSen AI Agent");
    lv_obj_set_style_text_font(app, &ui_font_sans_32, 0);
    lv_obj_set_style_text_color(app, lv_color_hex(0xffffff), 0);
    lv_obj_align(app, LV_ALIGN_TOP_MID, 0, 100);

    /* Info block */
    lv_obj_t *info = lv_label_create(about_page_container);
    lv_label_set_text(info,
                      "版本: v1.0.0\n"
                      "平台: SF32LB52 DevKit\n"
                      "队伍: Team 181\n"
                      "openvela AI 硬件大赛 2026");
    lv_obj_set_style_text_font(info, &ui_font_sans_16, 0);
    lv_obj_set_style_text_color(info, lv_color_hex(0xcccccc), 0);
    lv_obj_set_pos(info, 40, 220);
    lv_obj_set_width(info, 310);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);

    /* Hint */
    lv_obj_t *hint = lv_label_create(about_page_container);
    lv_label_set_text(hint, "右滑屏幕边缘或点击左上角返回");
    lv_obj_set_style_text_font(hint, &ui_font_sans_16, 0);
    lv_obj_set_style_text_color(hint, lv_color_hex(0x666688), 0);
    lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -30);

    printf("[About] About page created successfully\n");
    return about_page_container;
}

/**
 * Set the back button callback
 */
void about_page_set_back_callback(about_back_callback_t callback)
{
    back_callback = callback;
}

/**
 * Delete the about page resources
 */
void about_page_delete(void)
{
    printf("[About] Deleting about page\n");
    if (about_page_container != NULL)
    {
        lv_obj_del(about_page_container);
        about_page_container = NULL;
    }
    back_callback = NULL;
}
