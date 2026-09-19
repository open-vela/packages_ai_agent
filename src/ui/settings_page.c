/****************************************************************************
 * Settings Page
 *
 * Placeholder for LLM configuration and other settings.
 * Can be expanded in future iterations.
 *
 * Team 181 - Contest 2026
 ****************************************************************************/

#include <lvgl/lvgl.h>
#include <stdio.h>
#include <string.h>
#include "settings_page.h"

/* Sans-serif (Noto Sans SC) font family: full GB2312 CJK coverage.
 * (lv_font_simsun_16_cjk is only a small subset -> missing glyphs -> 乱码) */
extern const lv_font_t ui_font_sans_16;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static lv_obj_t *settings_page_container;
static lv_obj_t *llm_host_label;
static lv_obj_t *llm_model_label;
static lv_obj_t *llm_key_label;
static pet_back_callback_t back_callback = NULL;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/**
 * Handle back button click
 */
static void on_back_clicked(lv_event_t *e)
{
    printf("[Settings] Back button clicked\n");
    if (back_callback != NULL)
    {
        back_callback();
    }
}

/**
 * Create a settings row with label and value
 */
static lv_obj_t *create_settings_row(lv_obj_t *parent, const char *label_text,
                                      const char *value_text, lv_coord_t y)
{
    /* Label */

    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, label_text);
    lv_obj_set_style_text_font(label, &ui_font_sans_16, 0);
    lv_obj_set_style_text_color(label, lv_color_hex(0xcccccc), 0);
    lv_obj_set_pos(label, 20, y);

    /* Value (read-only for now) */

    lv_obj_t *value = lv_label_create(parent);
    lv_label_set_text(value, value_text);
    lv_obj_set_style_text_font(value, &ui_font_sans_16, 0);
    lv_obj_set_style_text_color(value, lv_color_hex(0x88ccff), 0);
    lv_obj_set_pos(value, 120, y);

    return value;
}

/****************************************************************************
 * Public API
 ****************************************************************************/

/**
 * Create the settings page
 *
 * @return Settings page container object, or NULL on failure
 */
lv_obj_t *settings_page_create(void)
{
    printf("[Settings] Creating settings page\n");

    settings_page_container = lv_obj_create(lv_scr_act());
    lv_obj_set_size(settings_page_container, 390, 450);
    lv_obj_set_pos(settings_page_container, 0, 0);
    lv_obj_set_style_bg_color(settings_page_container, lv_color_hex(0x1a1a2e), 0);
    lv_obj_set_style_border_width(settings_page_container, 0, 0);
    lv_obj_set_style_pad_all(settings_page_container, 20, 0);

    /* Back button (enlarged for easy tapping) */

    lv_obj_t *back_btn = lv_btn_create(settings_page_container);
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

    lv_obj_t *title = lv_label_create(settings_page_container);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_font(title, &ui_font_sans_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0x88ccff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 15);

    /* LLM Configuration Section */

    lv_obj_t *section = lv_label_create(settings_page_container);
    lv_label_set_text(section, "— LLM 配置 —");
    lv_obj_set_style_text_font(section, &ui_font_sans_16, 0);
    lv_obj_set_style_text_color(section, lv_color_hex(0xffaa00), 0);
    lv_obj_set_pos(section, 20, 60);

    /* LLM Host */

    llm_host_label = create_settings_row(settings_page_container,
                                          "服务地址:", "api.stepfun.com",
                                          90);

    /* LLM Model */

    llm_model_label = create_settings_row(settings_page_container,
                                          "模型:", "step-3.7-flash",
                                          120);

    /* LLM API Key (masked) */

    llm_key_label = create_settings_row(settings_page_container,
                                        "API Key:", "1k8F****",
                                        150);

    /* Info section */

    lv_obj_t *info = lv_label_create(settings_page_container);
    lv_label_set_text(info,
                      "配置说明:\n"
                      "在 NSH 中使用 set_llm 命令配置\n"
                      "格式: set_llm <host> <model> <key>\n"
                      "例: set_llm https://api.stepfun.com/v1 \\\n"
                      "      step-3.7-flash <your-key>");
    lv_obj_set_style_text_font(info, &ui_font_sans_16, 0);
    lv_obj_set_style_text_color(info, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_pos(info, 20, 200);
    lv_obj_set_width(info, 350);
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);

    /* Note（不用 ⚠️ emoji：ui_font_sans_16 不含 U+26A0，会乱码） */

    lv_obj_t *note = lv_label_create(settings_page_container);
    lv_label_set_text(note, "当前为只读展示，实际配置请在 NSH 中输入命令");
    lv_obj_set_style_text_font(note, &ui_font_sans_16, 0);
    lv_obj_set_style_text_color(note, lv_color_hex(0xffaa00), 0);
    lv_obj_align(note, LV_ALIGN_BOTTOM_MID, 0, -30);

    printf("[Settings] Settings page created successfully\n");
    return settings_page_container;
}

/**
 * Set the back button callback
 *
 * @param callback Function to call when back button is pressed
 */
void settings_page_set_back_callback(pet_back_callback_t callback)
{
    back_callback = callback;
}

/**
 * Update LLM host display
 *
 * @param host LLM host URL
 */
void settings_page_update_host(const char *host)
{
    if (llm_host_label != NULL && host != NULL)
    {
        lv_label_set_text(llm_host_label, host);
        lv_timer_handler();
    }
}

/**
 * Update LLM model display
 *
 * @param model LLM model name
 */
void settings_page_update_model(const char *model)
{
    if (llm_model_label != NULL && model != NULL)
    {
        lv_label_set_text(llm_model_label, model);
        lv_timer_handler();
    }
}

/**
 * Update API key display (masked)
 *
 * @param key API key (will be masked to first 4 chars + ****)
 */
void settings_page_update_key(const char *key)
{
    if (llm_key_label != NULL && key != NULL)
    {
        /* Mask key for display (show first 4 + ****) */

        if (strlen(key) > 4)
        {
            char masked[32];
            snprintf(masked, sizeof(masked), "%.4s****", key);
            lv_label_set_text(llm_key_label, masked);
        }
        else
        {
            lv_label_set_text(llm_key_label, "****");
        }
        lv_timer_handler();
    }
}

/**
 * Clean up settings page resources
 */
void settings_page_delete(void)
{
    printf("[Settings] Deleting settings page\n");
    if (settings_page_container != NULL)
    {
        lv_obj_del(settings_page_container);
        settings_page_container = NULL;
    }
    back_callback = NULL;
}
