#pragma once
#include "lvgl.h"
#include "bt_music_backend.h"

typedef void (*scan_connect_cb_t)(const bt_device_info_t *dev);

void bt_scan_ui_show(lv_obj_t *parent, scan_connect_cb_t connect_cb);
void bt_scan_ui_destroy(void);
void bt_scan_ui_add_device(const bt_device_info_t *dev);
void bt_scan_ui_set_status(const char *msg);
