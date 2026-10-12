#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stddef.h>
typedef void (*launcher_dialog_action_t)(void *arg);
void launcher_dialogs_init(lv_obj_t *screen);
/* UI-thread calls. One preallocated window is reused. */
void launcher_dialog_message(const char *title, const char *message);
void launcher_dialog_confirm(const char *title, const char *message,
                             launcher_dialog_action_t action, void *arg);
bool launcher_dialog_busy(void);
/* Worker-safe messages; no LVGL access in these functions. */
void launcher_dialog_progress(const char *title, const char *name, size_t done, size_t total);
void launcher_dialog_result(const char *title, const char *message, bool refresh_apps);
void launcher_dialog_set_refresh_callback(void (*callback)(void));
