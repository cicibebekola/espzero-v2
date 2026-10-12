#pragma once
/**
 * pulldown_bar.h — Global pull-down status bar (like phone notification shade)
 *
 * Shows: brightness slider, battery %, SD status, WiFi status.
 * Pull down from top edge to reveal. Touch-friendly, works across all apps.
 *
 * Usage:
 *   pulldown_bar_init();           // after lvgl_runtime_init()
 *   pulldown_bar_update_battery(); // call periodically or from timer
 */
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create the pull-down bar. Call once after LVGL init.
 * @param parent  Usually lv_screen_active()
 */
void pulldown_bar_init(lv_obj_t *parent);

/**
 * @brief Update battery display (call from timer).
 */
void pulldown_bar_update_battery(void);

/**
 * @brief Show or hide the bar programmatically.
 */
void pulldown_bar_show(bool show);

/**
 * @brief Get the slider widget (for external brightness control).
 */
lv_obj_t *pulldown_bar_get_brightness_slider(void);

#ifdef __cplusplus
}
#endif
