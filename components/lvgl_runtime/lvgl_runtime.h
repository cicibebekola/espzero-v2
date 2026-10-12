#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "esp_timer.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialise LVGL 9 display (esp_lcd ST7789), touch (esp_lcd XPT2046),
 *        and the periodic tick timer.
 *
 * Default orientation is native portrait (240×320, no swap/mirror).
 * Call lvgl_runtime_set_rotation() after init to change it.
 */
esp_err_t lvgl_runtime_init(uint32_t tick_inc_ms,
                            uint32_t tick_period_ms,
                            esp_timer_handle_t *out_tick_timer);

/**
 * @brief Change display orientation at runtime.
 *
 * @param swap_xy   Swap X/Y axes (landscape ↔ portrait)
 * @param mirror_x  Mirror the X axis
 * @param mirror_y  Mirror the Y axis
 */
esp_err_t lvgl_runtime_set_rotation(bool swap_xy, bool mirror_x, bool mirror_y);

/**
 * @brief Set backlight brightness (0-100%).
 */
esp_err_t lvgl_runtime_set_brightness(uint8_t percent);

/**
 * @brief Run the LVGL handler loop forever (call from app_main).
 */
void lvgl_runtime_run(uint32_t handler_period_ms);

#ifdef __cplusplus
}
#endif
