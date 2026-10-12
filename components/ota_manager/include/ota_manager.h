#pragma once
/**
 * ota_manager.h — OTA app manager UI
 *
 * Lists .bin files from /sdcard/apps as horizontal cards.
 * Each card shows: app name, file size, flash button, delete button.
 * Flash calls ota_loader to write to OTA partition and reboot.
 */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the OTA manager UI inside a parent container.
 * @param parent  Container to build the UI in
 */
void ota_manager_init(lv_obj_t *parent);

/**
 * @brief Refresh the app list (rescan SD card).
 */
void ota_manager_refresh(void);

#ifdef __cplusplus
}
#endif
