#include "pulldown_bar.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "driver/gpio.h"
#include "lvgl_runtime.h"
#include "sleep_manager.h"
#include "sd_card_manager.h"
#include "board_config.h"

static const char *TAG = "pulldown";

/* State */
static lv_obj_t *s_container = NULL;
static lv_obj_t *s_panel = NULL;
static lv_obj_t *s_handle = NULL;
static lv_obj_t *s_lbl_battery = NULL;
static lv_obj_t *s_bar_battery = NULL;
static lv_obj_t *s_lbl_sd = NULL;
static lv_obj_t *s_slider_brightness = NULL;
static lv_obj_t *s_lbl_brightness = NULL;
static bool s_is_open = false;
static lv_coord_t s_drag_start_y = 0;

/* Colors */
#define COL_BG          lv_color_hex(0x1a1a2e)
#define COL_CARD        lv_color_hex(0x16213e)
#define COL_PRIMARY     lv_color_hex(0x0f3460)
#define COL_ACCENT      lv_color_hex(0xe94560)
#define COL_TEXT        lv_color_hex(0xeaeaea)
#define COL_TEXT_DIM    lv_color_hex(0x8899aa)
#define COL_GREEN       lv_color_hex(0x53d769)
#define COL_CYAN        lv_color_hex(0x4fc3f7)

/* Panel dimensions */
#define PANEL_W     240
#define PANEL_H     175
#define HANDLE_H    12

/* ── Brightness slider callback ── */
static void on_brightness_changed(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int val = lv_slider_get_value(slider);
    lvgl_runtime_set_brightness((uint8_t)val);
    lv_label_set_text_fmt(s_lbl_brightness, "%d%%", val);
    sleep_manager_activity();
}

/* ── LED toggle ── */
static bool s_led_on = false;
static lv_obj_t *s_btn_led = NULL;

static void on_led_click(lv_event_t *e)
{
    (void)e;
    s_led_on = !s_led_on;
    gpio_set_direction(BOARD_LED_GREEN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_LED_GREEN_GPIO, s_led_on ? 0 : 1);

    if (s_btn_led) {
        lv_obj_t *lbl = lv_obj_get_child(s_btn_led, 0);
        if (s_led_on) {
            lv_obj_set_style_bg_color(s_btn_led, lv_color_hex(0x1a4a2e), 0);
            lv_obj_set_style_border_color(s_btn_led, COL_GREEN, 0);
            if (lbl) lv_obj_set_style_text_color(lbl, COL_GREEN, 0);
        } else {
            lv_obj_set_style_bg_color(s_btn_led, COL_CARD, 0);
            lv_obj_set_style_border_color(s_btn_led, lv_color_hex(0x334455), 0);
            if (lbl) lv_obj_set_style_text_color(lbl, COL_TEXT_DIM, 0);
        }
    }
}

/* ── Close button ── */
static void on_close_click(lv_event_t *e)
{
    (void)e;
    pulldown_bar_show(false);
}

/* ── Touch events on the handle strip ── */
static void on_handle_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    s_drag_start_y = p.y;
}

static void on_handle_released(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    if (s_is_open) {
        /* Swipe up to close */
        if (p.y < s_drag_start_y - 30) {
            pulldown_bar_show(false);
        }
    } else {
        /* Swipe down to open */
        if (p.y > s_drag_start_y + 30) {
            pulldown_bar_show(true);
        }
    }
}

static void on_handle_click(lv_event_t *e)
{
    (void)e;
    /* Toggle on click (fallback for short taps) */
    pulldown_bar_show(!s_is_open);
}

/* ── Build the panel ── */
static void build_panel(void)
{
    s_panel = lv_obj_create(s_container);
    lv_obj_set_size(s_panel, PANEL_W, PANEL_H);
    lv_obj_align(s_panel, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_panel, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 8, 0);
    lv_obj_set_style_clip_corner(s_panel, true, 0);

    /* Title row */
    lv_obj_t *title_row = lv_obj_create(s_panel);
    lv_obj_set_size(title_row, PANEL_W - 16, 30);
    lv_obj_set_pos(title_row, 0, 0);
    lv_obj_set_style_bg_opa(title_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(title_row, 0, 0);
    lv_obj_set_style_pad_all(title_row, 0, 0);

    lv_obj_t *title = lv_label_create(title_row);
    lv_label_set_text(title, LV_SYMBOL_SETTINGS " Quick Settings");
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_14, 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    /* Close button */
    lv_obj_t *btn_close = lv_btn_create(title_row);
    lv_obj_set_size(btn_close, 30, 24);
    lv_obj_align(btn_close, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_close, COL_ACCENT, 0);
    lv_obj_set_style_radius(btn_close, 6, 0);
    lv_obj_set_style_border_width(btn_close, 0, 0);
    lv_obj_set_style_pad_all(btn_close, 0, 0);
    lv_obj_add_event_cb(btn_close, on_close_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_close = lv_label_create(btn_close);
    lv_label_set_text(lbl_close, LV_SYMBOL_CLOSE);
    lv_obj_set_style_text_color(lbl_close, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_close, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_close);

    /* ── Battery section ── */
    lv_obj_t *bat_card = lv_obj_create(s_panel);
    lv_obj_set_size(bat_card, PANEL_W - 16, 50);
    lv_obj_set_pos(bat_card, 0, 32);
    lv_obj_set_style_bg_color(bat_card, COL_CARD, 0);
    lv_obj_set_style_bg_opa(bat_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bat_card, 8, 0);
    lv_obj_set_style_border_width(bat_card, 0, 0);
    lv_obj_set_style_pad_all(bat_card, 6, 0);

    s_lbl_battery = lv_label_create(bat_card);
    lv_label_set_text(s_lbl_battery, LV_SYMBOL_BATTERY_FULL " Battery");
    lv_obj_set_style_text_color(s_lbl_battery, COL_TEXT, 0);
    lv_obj_set_style_text_font(s_lbl_battery, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_battery, LV_ALIGN_TOP_LEFT, 0, 0);

    s_bar_battery = lv_bar_create(bat_card);
    lv_obj_set_size(s_bar_battery, PANEL_W - 36, 14);
    lv_obj_align(s_bar_battery, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(s_bar_battery, 0, 100);
    lv_bar_set_value(s_bar_battery, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar_battery, lv_color_hex(0x2a2a3e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar_battery, COL_GREEN, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar_battery, 7, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar_battery, 7, LV_PART_INDICATOR);

    /* ── Brightness section ── */
    lv_obj_t *bright_card = lv_obj_create(s_panel);
    lv_obj_set_size(bright_card, PANEL_W - 16, 50);
    lv_obj_set_pos(bright_card, 0, 82);
    lv_obj_set_style_bg_color(bright_card, COL_CARD, 0);
    lv_obj_set_style_bg_opa(bright_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bright_card, 8, 0);
    lv_obj_set_style_border_width(bright_card, 0, 0);
    lv_obj_set_style_pad_all(bright_card, 6, 0);

    lv_obj_t *bright_icon = lv_label_create(bright_card);
    lv_label_set_text(bright_icon, LV_SYMBOL_SETTINGS " Brightness");
    lv_obj_set_style_text_color(bright_icon, COL_TEXT, 0);
    lv_obj_set_style_text_font(bright_icon, &lv_font_montserrat_14, 0);
    lv_obj_align(bright_icon, LV_ALIGN_TOP_LEFT, 0, 0);

    s_lbl_brightness = lv_label_create(bright_card);
    lv_label_set_text(s_lbl_brightness, "80%");
    lv_obj_set_style_text_color(s_lbl_brightness, COL_ACCENT, 0);
    lv_obj_set_style_text_font(s_lbl_brightness, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_brightness, LV_ALIGN_TOP_RIGHT, 0, 0);

    s_slider_brightness = lv_slider_create(bright_card);
    lv_obj_set_size(s_slider_brightness, PANEL_W - 36, 10);
    lv_obj_align(s_slider_brightness, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_slider_set_range(s_slider_brightness, 10, 100);
    lv_slider_set_value(s_slider_brightness, 80, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_slider_brightness, lv_color_hex(0x2a2a3e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider_brightness, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider_brightness, COL_TEXT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_slider_brightness, 4, LV_PART_KNOB);
    lv_obj_add_event_cb(s_slider_brightness, on_brightness_changed, LV_EVENT_VALUE_CHANGED, NULL);

    /* ── Status row (SD, LED) ── */
    lv_obj_t *status_card = lv_obj_create(s_panel);
    lv_obj_set_size(status_card, PANEL_W - 16, 40);
    lv_obj_set_pos(status_card, 0, 132);
    lv_obj_set_style_bg_color(status_card, COL_CARD, 0);
    lv_obj_set_style_bg_opa(status_card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(status_card, 8, 0);
    lv_obj_set_style_border_width(status_card, 0, 0);
    lv_obj_set_style_pad_all(status_card, 6, 0);

    s_lbl_sd = lv_label_create(status_card);
    lv_label_set_text(s_lbl_sd, LV_SYMBOL_SD_CARD " SD");
    lv_obj_set_style_text_color(s_lbl_sd, COL_CYAN, 0);
    lv_obj_set_style_text_font(s_lbl_sd, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_sd, LV_ALIGN_LEFT_MID, 0, 0);

    /* LED toggle button */
    s_btn_led = lv_btn_create(status_card);
    lv_obj_set_size(s_btn_led, 70, 28);
    lv_obj_align(s_btn_led, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_btn_led, COL_CARD, 0);
    lv_obj_set_style_radius(s_btn_led, 6, 0);
    lv_obj_set_style_border_width(s_btn_led, 1, 0);
    lv_obj_set_style_border_color(s_btn_led, lv_color_hex(0x334455), 0);
    lv_obj_add_event_cb(s_btn_led, on_led_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_led = lv_label_create(s_btn_led);
    lv_label_set_text(lbl_led, LV_SYMBOL_POWER " LED");
    lv_obj_set_style_text_color(lbl_led, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl_led, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_led);
}

/* ── Public API ── */

void pulldown_bar_init(lv_obj_t *parent)
{
    /* Container covers full screen, but is transparent and pass-through */
    s_container = lv_obj_create(parent);
    lv_obj_set_size(s_container, PANEL_W, PANEL_H + HANDLE_H);
    lv_obj_set_pos(s_container, 0, -(PANEL_H));  /* hidden above screen */
    lv_obj_set_style_bg_color(s_container, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_container, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_container, 0, 0);
    lv_obj_set_style_radius(s_container, 0, 0);
    lv_obj_set_style_pad_all(s_container, 0, 0);
    lv_obj_set_style_clip_corner(s_container, true, 0);
    lv_obj_clear_flag(s_container, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_anim_duration(s_container, 300, 0);

    /* Build the panel */
    build_panel();

    /* Handle strip — always visible at top of screen */
    s_handle = lv_obj_create(parent);
    lv_obj_set_size(s_handle, 80, HANDLE_H);
    lv_obj_align(s_handle, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(s_handle, lv_color_hex(0x333344), 0);
    lv_obj_set_style_bg_opa(s_handle, LV_OPA_60, 0);
    lv_obj_set_style_radius(s_handle, 0, 0);
    lv_obj_set_style_border_width(s_handle, 0, 0);
    lv_obj_clear_flag(s_handle, LV_OBJ_FLAG_SCROLLABLE);

    /* Handle grip indicator */
    lv_obj_t *grip = lv_obj_create(s_handle);
    lv_obj_set_size(grip, 30, 3);
    lv_obj_center(grip);
    lv_obj_set_style_bg_color(grip, lv_color_hex(0x888899), 0);
    lv_obj_set_style_bg_opa(grip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(grip, 2, 0);
    lv_obj_set_style_border_width(grip, 0, 0);
    lv_obj_clear_flag(grip, LV_OBJ_FLAG_SCROLLABLE);

    /* Touch events for pull-down */
    lv_obj_add_event_cb(s_handle, on_handle_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_handle, on_handle_released, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_handle, on_handle_click, LV_EVENT_SHORT_CLICKED, NULL);

    /* Initial state: hidden */
    pulldown_bar_show(false);

    ESP_LOGI(TAG, "Pull-down bar initialized");
}

void pulldown_bar_update_battery(void)
{
    if (!s_lbl_battery || !s_bar_battery) return;

    uint8_t pct = sleep_manager_get_battery_percent();
    uint32_t mv = sleep_manager_get_battery_voltage();

    lv_bar_set_value(s_bar_battery, pct, LV_ANIM_ON);

    if (pct > 50) {
        lv_obj_set_style_bg_color(s_bar_battery, COL_GREEN, LV_PART_INDICATOR);
    } else if (pct > 20) {
        lv_obj_set_style_bg_color(s_bar_battery, lv_color_hex(0xFFCC00), LV_PART_INDICATOR);
    } else {
        lv_obj_set_style_bg_color(s_bar_battery, COL_ACCENT, LV_PART_INDICATOR);
    }

    lv_label_set_text_fmt(s_lbl_battery,
        LV_SYMBOL_BATTERY_FULL " Battery: %d%% (%lu mV)",
        (int)pct, (unsigned long)mv);
}

void pulldown_bar_show(bool show)
{
    if (!s_container) return;

    s_is_open = show;

    if (show) {
        /* Slide down */
        lv_obj_set_pos(s_container, 0, 0);
        lv_obj_set_style_bg_opa(s_container, LV_OPA_90, 0);
    } else {
        /* Slide up */
        lv_obj_set_pos(s_container, 0, -(PANEL_H));
        lv_obj_set_style_bg_opa(s_container, LV_OPA_TRANSP, 0);
    }
}

lv_obj_t *pulldown_bar_get_brightness_slider(void)
{
    return s_slider_brightness;
}
