#include "bt_scan_ui.h"
#include <string.h>
#include <stdio.h>
#include "esp_log.h"

#define TAG "BT_SCAN_UI"

#define COL_BG      lv_color_hex(0x12121F)
#define COL_CARD    lv_color_hex(0x1A1A2E)
#define COL_ROW     lv_color_hex(0x16213E)
#define COL_CYAN    lv_color_hex(0x4FC3F7)
#define COL_TEXT    lv_color_hex(0xEEEEEE)
#define COL_DIM     lv_color_hex(0x666688)

static lv_obj_t *s_screen     = NULL;
static lv_obj_t *s_scroll     = NULL;
static lv_obj_t *s_status_lbl = NULL;
static scan_connect_cb_t s_connect_cb = NULL;

#define MAX_ROWS 12
static bt_device_info_t s_devices[MAX_ROWS];
static int s_device_count = 0;

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    bt_scan_ui_destroy();
}

static void device_btn_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_device_count) return;
    if (s_connect_cb)
        s_connect_cb(&s_devices[idx]);
}

void bt_scan_ui_show(lv_obj_t *parent, scan_connect_cb_t connect_cb)
{
    s_connect_cb = connect_cb;
    s_device_count = 0;

    s_screen = lv_obj_create(parent);
    lv_obj_set_size(s_screen, 240, 320);
    lv_obj_align(s_screen, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_screen, COL_BG, 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_move_foreground(s_screen);

    /* Title */
    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 20);
    lv_obj_set_style_text_color(title, COL_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_label_set_text(title, LV_SYMBOL_BLUETOOTH "  Find Headphones");

    /* Status */
    s_status_lbl = lv_label_create(s_screen);
    lv_obj_align(s_status_lbl, LV_ALIGN_TOP_MID, 0, 48);
    lv_obj_set_style_text_color(s_status_lbl, COL_CYAN, 0);
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_status_lbl, "Scanning...");

    /* Device list */
    s_scroll = lv_obj_create(s_screen);
    lv_obj_set_size(s_scroll, 232, 210);
    lv_obj_align(s_scroll, LV_ALIGN_TOP_MID, 0, 68);
    lv_obj_set_style_bg_color(s_scroll, COL_CARD, 0);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_scroll, 1, 0);
    lv_obj_set_style_border_color(s_scroll, lv_color_hex(0x333355), 0);
    lv_obj_set_style_radius(s_scroll, 8, 0);
    lv_obj_set_style_pad_all(s_scroll, 4, 0);
    lv_obj_set_style_pad_row(s_scroll, 4, 0);
    lv_obj_set_flex_flow(s_scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);

    /* Back button */
    lv_obj_t *back_btn = lv_btn_create(s_screen);
    lv_obj_set_size(back_btn, 220, 36);
    lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_style_bg_color(back_btn, lv_color_hex(0x333355), 0);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_add_event_cb(back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_color(back_lbl, COL_TEXT, 0);
    lv_obj_center(back_lbl);
}

void bt_scan_ui_destroy(void)
{
    if (s_screen) {
        lv_obj_delete(s_screen);
        s_screen = NULL;
        s_scroll = NULL;
        s_status_lbl = NULL;
        s_device_count = 0;
    }
}

void bt_scan_ui_add_device(const bt_device_info_t *dev)
{
    if (!s_scroll || s_device_count >= MAX_ROWS) return;

    /* Deduplicate */
    for (int i = 0; i < s_device_count; i++) {
        if (memcmp(s_devices[i].addr, dev->addr, 6) == 0)
            return;
    }

    int idx = s_device_count;
    memcpy(&s_devices[idx], dev, sizeof(bt_device_info_t));
    s_device_count++;

    /* Row button */
    lv_obj_t *btn = lv_btn_create(s_scroll);
    lv_obj_set_size(btn, 220, 44);
    lv_obj_set_style_bg_color(btn, COL_ROW, 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0x2A2A50), 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_pad_all(btn, 4, 0);
    lv_obj_add_event_cb(btn, device_btn_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)idx);

    /* Icon */
    lv_obj_t *icon = lv_label_create(btn);
    lv_obj_align(icon, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_set_style_text_color(icon, COL_CYAN, 0);
    lv_label_set_text(icon, LV_SYMBOL_AUDIO);

    /* Name */
    lv_obj_t *name_lbl = lv_label_create(btn);
    lv_obj_set_width(name_lbl, 170);
    lv_obj_align(name_lbl, LV_ALIGN_LEFT_MID, 28, -8);
    lv_obj_set_style_text_color(name_lbl, COL_TEXT, 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(name_lbl, dev->name);

    /* RSSI */
    lv_obj_t *rssi_lbl = lv_label_create(btn);
    lv_obj_align(rssi_lbl, LV_ALIGN_LEFT_MID, 28, 8);
    lv_obj_set_style_text_color(rssi_lbl, COL_DIM, 0);
    lv_obj_set_style_text_font(rssi_lbl, &lv_font_montserrat_14, 0);
    lv_label_set_text_fmt(rssi_lbl, "%d dBm", dev->rssi);
}

void bt_scan_ui_set_status(const char *msg)
{
    if (s_status_lbl)
        lv_label_set_text(s_status_lbl, msg);
}
