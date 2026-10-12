#include "launcher_dialogs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <stdint.h>
#include <string.h>

typedef struct {
    char title[40];
    char message[160];
    size_t done, total;
    bool progress, refresh;
} dialog_event_t;
static QueueHandle_t s_events;
static lv_obj_t *s_shade, *s_title, *s_message, *s_bar, *s_ok, *s_cancel, *s_ok_label;
static launcher_dialog_action_t s_action;
static void *s_arg;
static bool s_busy;
static void (*s_refresh)(void);

static void close_clicked(lv_event_t *event)
{
    bool accept = lv_event_get_user_data(event) != NULL;
    launcher_dialog_action_t action = s_action;
    void *arg = s_arg;
    s_action = NULL;
    s_busy = false;
    lv_obj_set_hidden(s_shade, true);
    if (accept && action != NULL) action(arg);
}

static void show(const char *title, const char *message, bool progress)
{
    if (s_shade == NULL) return;
    s_action = NULL;
    lv_label_set_text(s_title, title);
    lv_label_set_text(s_message, message);
    lv_obj_set_hidden(s_bar, !progress);
    lv_obj_set_hidden(s_ok, progress);
    lv_obj_set_hidden(s_cancel, true);
    lv_label_set_text(s_ok_label, "OK");
    lv_obj_align(s_ok, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_hidden(s_shade, false);
    lv_obj_move_foreground(s_shade);
    s_busy = progress;
}

void launcher_dialog_message(const char *title, const char *message)
{
    show(title, message, false);
}

void launcher_dialog_confirm(const char *title, const char *message,
                             launcher_dialog_action_t action, void *arg)
{
    if (s_busy) return;
    show(title, message, false);
    s_action = action;
    s_arg = arg;
    s_busy = true;
    lv_label_set_text(s_ok_label, "Continue");
    lv_obj_align(s_ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_hidden(s_cancel, false);
}

bool launcher_dialog_busy(void) { return s_busy; }
void launcher_dialog_set_refresh_callback(void (*callback)(void)) { s_refresh = callback; }

static void poll(lv_timer_t *timer)
{
    (void)timer;
    dialog_event_t event;
    while (xQueueReceive(s_events, &event, 0) == pdTRUE) {
        show(event.title, event.message, event.progress);
        if (event.progress) {
            lv_bar_set_value(s_bar, event.total ? (int32_t)((uint64_t)event.done * 100 / event.total) : 0,
                             LV_ANIM_OFF);
        }
        if (event.refresh && s_refresh != NULL) s_refresh();
    }
}

void launcher_dialog_progress(const char *title, const char *name, size_t done, size_t total)
{
    if (s_events == NULL) return;
    dialog_event_t event = {.done = done, .total = total, .progress = true};
    snprintf(event.title, sizeof(event.title), "%s", title);
    snprintf(event.message, sizeof(event.message), "%s\n%u%%", name,
             total ? (unsigned)((uint64_t)done * 100 / total) : 0);
    xQueueSend(s_events, &event, 0);
}

void launcher_dialog_result(const char *title, const char *message, bool refresh_apps)
{
    if (s_events == NULL) return;
    dialog_event_t event = {.refresh = refresh_apps};
    snprintf(event.title, sizeof(event.title), "%s", title);
    snprintf(event.message, sizeof(event.message), "%s", message);
    /* Keep terminal events: discard old progress if the queue is full. */
    if (xQueueSend(s_events, &event, 0) != pdTRUE) {
        dialog_event_t discarded;
        xQueueReceive(s_events, &discarded, 0);
        xQueueSend(s_events, &event, 0);
    }
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, bool accept)
{
    lv_obj_t *obj = lv_button_create(parent);
    lv_obj_set_size(obj, 94, 36);
    lv_obj_set_style_bg_color(obj, lv_color_hex(accept ? 0x238636 : 0x484848), 0);
    lv_obj_t *label = lv_label_create(obj);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (accept) s_ok_label = label;
    lv_obj_add_event_cb(obj, close_clicked, LV_EVENT_CLICKED, accept ? (void *)1 : NULL);
    return obj;
}

void launcher_dialogs_init(lv_obj_t *screen)
{
    s_events = xQueueCreate(8, sizeof(dialog_event_t));
    if (s_events == NULL) return;
    s_shade = lv_obj_create(screen);
    lv_obj_set_size(s_shade, 240, 320);
    lv_obj_set_pos(s_shade, 0, 0);
    lv_obj_set_style_pad_all(s_shade, 0, 0);
    lv_obj_set_style_border_width(s_shade, 0, 0);
    lv_obj_set_style_radius(s_shade, 0, 0);
    lv_obj_set_style_bg_color(s_shade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_shade, LV_OPA_60, 0);
    lv_obj_set_scrollable(s_shade, false);
    lv_obj_t *window = lv_obj_create(s_shade);
    lv_obj_set_size(window, 224, 200);
    lv_obj_center(window);
    lv_obj_set_style_bg_color(window, lv_color_hex(0x282828), 0);
    lv_obj_set_style_border_color(window, lv_color_hex(0x666666), 0);
    lv_obj_set_style_pad_all(window, 10, 0);
    lv_obj_set_style_text_color(window, lv_color_white(), 0);
    lv_obj_set_scrollable(window, false);
    s_title = lv_label_create(window);
    lv_obj_set_width(s_title, 200);
    lv_obj_align(s_title, LV_ALIGN_TOP_LEFT, 0, 0);
    s_message = lv_label_create(window);
    lv_obj_set_width(s_message, 200);
    lv_obj_set_height(s_message, 90);
    lv_label_set_long_mode(s_message, LV_LABEL_LONG_DOT);
    lv_obj_align(s_message, LV_ALIGN_TOP_LEFT, 0, 28);
    s_bar = lv_bar_create(window);
    lv_obj_set_size(s_bar, 200, 12);
    lv_obj_align(s_bar, LV_ALIGN_BOTTOM_MID, 0, -44);
    s_ok = button(window, "OK", true);
    s_cancel = button(window, "Cancel", false);
    lv_obj_align(s_cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_hidden(s_shade, true);
    lv_timer_create(poll, 100, NULL);
}
