#pragma once
/**
 * ota_loader.h
 *
 * Stream a firmware binary from the filesystem (SD) into the next OTA partition.
 */

#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Select the saved launcher partition (legacy installs use ota_0).
 * Returns an error instead of rebooting; caller stops playback and restarts. */
esp_err_t ota_loader_select_launcher(void);


/**
 * NOTE:
 * SD card must already be mounted before calling this function,
 * OR this module will attempt auto-mount if supported.
 */

/** Progress callback:
 *  written_bytes = how many bytes have been written so far
 *  total_bytes = total file size from filesystem (0 if unknown)
 *  arg = user pointer passed through
 */
typedef void (*ota_progress_cb_t)(size_t written_bytes, size_t total_bytes, void *arg);

/**
 * Stream & flash a file from the filesystem into the next update partition.
 *
 * - sd_path: path to binary (e.g. "/sdcard/apps/myapp/app.bin")
 * - progress_cb: callback called occasionally with progress (may be NULL)
 * - cb_arg: user pointer passed to progress_cb
 * - reboot_on_success: if true, function will call esp_restart() after setting boot partition
 *
 * This function is blocking and should be called from a FreeRTOS task (not the LVGL task).
 * The progress_cb must be lightweight — post a message to GUI queue instead of performing LVGL calls.
 *
 * Returns ESP_OK on success (but if reboot_on_success==true the function will not return).
 */
esp_err_t ota_loader_flash_sd(const char *sd_path,
                             ota_progress_cb_t progress_cb,
                             void *cb_arg,
                             bool reboot_on_success);

#ifdef __cplusplus
}
#endif
