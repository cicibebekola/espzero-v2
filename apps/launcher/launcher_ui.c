#include "launcher_ui.h"
#include "ota_manager.h"
#include "file_server.h"
#include "device_paths.h"
#include "launcher_dialogs.h"
#include "sleep_manager.h"
#include "sd_card_manager.h"
#include "esp_app_desc.h"
#include "esp_wifi.h"

#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define COLOR_PRIMARY  lv_color_hex(0x2196F3)
#define COLOR_BG_DARK  lv_color_hex(0x1E1E1E)
#define COLOR_BG_LIGHT lv_color_hex(0x2D2D2D)
#define COLOR_TEXT     lv_color_hex(0xFFFFFF)
#define COLOR_MUTED    lv_color_hex(0xA0A0A0)
#define MAX_SCAN_RESULTS 12
#define NETWORK_ROWS 4

static lv_obj_t *s_title;
static lv_obj_t *s_apps_view;
static lv_obj_t *s_wifi_view;
static lv_obj_t *s_nav_label;
static lv_obj_t *s_status;
static lv_obj_t *s_connect_btn;
static lv_obj_t *s_scan_btn;
static lv_obj_t *s_password;
static lv_obj_t *s_keyboard;
static lv_obj_t *s_password_popup, *s_password_title, *s_menu;
static lv_obj_t *s_wifi_icon, *s_battery;
static bool s_user_connect;
static lv_obj_t *s_network_buttons[NETWORK_ROWS];
static lv_obj_t *s_network_labels[NETWORK_ROWS];
static file_server_wifi_network_t s_networks[MAX_SCAN_RESULTS];
static size_t s_network_count;
static size_t s_network_page;
static lv_obj_t *s_more_networks;
static char s_selected_ssid[33];
static bool s_showing_wifi;
static bool s_connecting;
static bool s_scanning;
static bool s_scanned_once;

static void start_scan(void);
static void connect_clicked(lv_event_t *event);

static void set_status(const char *text)
{
    if (s_status != NULL) {
        lv_label_set_text(s_status, text);
    }
}

static void show_wifi(bool show)
{
    s_showing_wifi = show;
    if (show) {
        lv_obj_set_hidden(s_apps_view, true);
        lv_obj_set_hidden(s_wifi_view, false);
        if (!s_scanned_once && !s_connecting) {
            start_scan();
        }
    } else {
        lv_obj_set_hidden(s_wifi_view, true);
        lv_obj_set_hidden(s_apps_view, false);
        lv_obj_set_hidden(s_password_popup, true);
    }
}

static void keyboard_event(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) {
        if (code == LV_EVENT_READY) connect_clicked(NULL);
        else {
            lv_textarea_set_text(s_password, "");
            lv_obj_set_hidden(s_password_popup, true);
        }
    }
}

static void textarea_focused(lv_event_t *event)
{
    lv_obj_t *textarea = lv_event_get_target_obj(event);
    lv_keyboard_set_textarea(s_keyboard, textarea);
    lv_obj_set_hidden(s_keyboard, false);
    lv_obj_move_foreground(s_keyboard);
}

static void nav_clicked(lv_event_t *event)
{
    (void)event;
    lv_obj_set_hidden(s_menu, !lv_obj_is_hidden(s_menu));
    lv_obj_move_foreground(s_menu);
}

static void connection_task(void *arg)
{
    (void)arg;
    if (file_server_is_running()) {
        file_server_stop();
    }
    esp_err_t result = file_server_start(DEVICE_SD_MOUNT_POINT);

    lvgl_port_lock(0);
    if (result == ESP_OK) {
        const char *ip = file_server_get_ip();
        char message[80];
        snprintf(message, sizeof(message), "Connected - %s", ip != NULL ? ip : "ready");
        set_status(message);
        if (s_user_connect) {
            wifi_ap_record_t ap;
            bool station = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
            launcher_dialog_message(station ? "Wi-Fi connected" : "Access point started",
                station ? message : "Could not join the network.\nConnect to EspZero\nPassword: 12345678\nFiles: 192.168.4.1");
        }
    } else {
        set_status("Connection failed");
        launcher_dialog_message("Connection failed", esp_err_to_name(result));
    }
    s_user_connect = false;
    lv_obj_remove_state(s_connect_btn, LV_STATE_DISABLED);
    s_connecting = false;
    if (s_showing_wifi && !s_scanned_once) {
        start_scan();
    }
    lvgl_port_unlock();
    vTaskDelete(NULL);
}

static void connect_clicked(lv_event_t *event)
{
    (void)event;
    if (s_connecting || s_scanning) {
        return;
    }

    const char *password = lv_textarea_get_text(s_password);
    if (s_selected_ssid[0] == '\0') {
        set_status("Select a network first");
        return;
    }

    if (file_server_save_wifi_credentials(s_selected_ssid, password) != ESP_OK) {
        launcher_dialog_message("Save failed", "Could not save Wi-Fi credentials.");
        return;
    }

    s_connecting = true;
    s_user_connect = true;
    lv_obj_set_hidden(s_password_popup, true);
    lv_textarea_set_text(s_password, "");
    lv_obj_add_state(s_connect_btn, LV_STATE_DISABLED);
    set_status("Connecting in station mode...");
    if (xTaskCreate(connection_task, "wifi_connect", 6144, NULL, 5, NULL) != pdPASS) {
        s_connecting = false;
        lv_obj_remove_state(s_connect_btn, LV_STATE_DISABLED);
        set_status("Could not start connection task");
        launcher_dialog_message("Connection failed", "Not enough memory to connect.");
    }
}

static void forget_clicked(lv_event_t *event)
{
    (void)event;
    if (s_connecting || s_scanning) return;
    if (file_server_clear_wifi_credentials() == ESP_OK) {
        s_selected_ssid[0] = '\0';
        lv_textarea_set_text(s_password, "");
        set_status("Saved network removed; AP fallback enabled");
    } else {
        set_status("Could not remove saved network");
    }
}

static void update_network_rows(void)
{
    for (size_t i = 0; i < NETWORK_ROWS; ++i) {
        size_t index = s_network_page * NETWORK_ROWS + i;
        if (index < s_network_count) {
            char row_text[64];
            snprintf(row_text, sizeof(row_text), "%s%s   %d dBm",
                     s_networks[index].secure ? LV_SYMBOL_WIFI " " : "",
                     s_networks[index].ssid, s_networks[index].rssi);
            lv_label_set_text(s_network_labels[i], row_text);
            lv_obj_set_style_bg_color(s_network_buttons[i],
                strcmp(s_selected_ssid, s_networks[index].ssid) == 0
                    ? COLOR_PRIMARY : COLOR_BG_LIGHT, 0);
            lv_obj_set_hidden(s_network_buttons[i], false);
        } else {
            lv_obj_set_hidden(s_network_buttons[i], true);
        }
    }
    lv_obj_set_hidden(s_more_networks, s_network_count <= NETWORK_ROWS);
}

static void more_networks(lv_event_t *event)
{
    (void)event;
    if (s_scanning || s_connecting) return;
    size_t pages = (s_network_count + NETWORK_ROWS - 1) / NETWORK_ROWS;
    if (pages > 0) s_network_page = (s_network_page + 1) % pages;
    update_network_rows();
}

static void network_clicked(lv_event_t *event)
{
    if (s_connecting || s_scanning) return;
    size_t index = s_network_page * NETWORK_ROWS + (size_t)(uintptr_t)lv_event_get_user_data(event);
    if (index >= s_network_count) return;

    snprintf(s_selected_ssid, sizeof(s_selected_ssid), "%s", s_networks[index].ssid);
    lv_textarea_set_text(s_password, "");
    lv_textarea_set_placeholder_text(s_password,
        s_networks[index].secure ? "Network password" : "Open network - no password");
    update_network_rows();

    char message[64];
    snprintf(message, sizeof(message), "Selected: %s", s_selected_ssid);
    set_status(message);
    lv_label_set_text(s_password_title, s_selected_ssid);
    lv_obj_set_hidden(s_keyboard, false);
    if (s_networks[index].secure) {
        lv_obj_set_hidden(s_password_popup, false);
        lv_obj_move_foreground(s_password_popup);
        lv_keyboard_set_textarea(s_keyboard, s_password);
    } else {
        connect_clicked(NULL);
    }
}

static void password_cancel(lv_event_t *event)
{
    (void)event;
    lv_textarea_set_text(s_password, "");
    lv_obj_set_hidden(s_password_popup, true);
}

static void menu_clicked(lv_event_t *event)
{
    int item = (int)(intptr_t)lv_event_get_user_data(event);
    lv_obj_set_hidden(s_menu, true);
    if (item == 0) show_wifi(!s_showing_wifi);
    else if (item == 1) {
        if (!sd_is_mounted()) launcher_dialog_message("No SD card", "Insert an SD card and restart to use files.");
        else if (file_server_is_running()) {
            char message[100];
            snprintf(message, sizeof(message), "Open in your browser:\nhttp://%s/\nChoose Apps for firmware uploads.", file_server_get_ip());
            launcher_dialog_message("Web file manager", message);
        } else launcher_dialog_message("Web file manager", "Connect Wi-Fi first using the menu.");
    } else ota_manager_refresh();
}

static void status_tick(lv_timer_t *timer)
{
    (void)timer;
    wifi_ap_record_t ap;
    bool connected = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
    lv_obj_set_style_text_color(s_wifi_icon, connected ? lv_color_hex(0x53D769) : COLOR_MUTED, 0);
    lv_label_set_text_fmt(s_battery, LV_SYMBOL_BATTERY_FULL " %u%%", sleep_manager_get_battery_percent());
}

static void transfer_event(const char *operation, const char *filename, size_t done,
                           size_t total, bool finished, bool success)
{
    char title[40];
    snprintf(title, sizeof(title), "%s %s", operation, finished ? (success ? "complete" : "failed") : "in progress");
    if (finished) launcher_dialog_result(title, filename, success && strcmp(operation, "Download") != 0);
    else launcher_dialog_progress(title, filename, done, total);
}

static void scan_task(void *arg)
{
    (void)arg;
    size_t count = 0;
    esp_err_t result = file_server_scan_wifi(s_networks, MAX_SCAN_RESULTS, &count);

    lvgl_port_lock(0);
    s_network_count = result == ESP_OK ? count : 0;
    s_network_page = 0;
    s_scanning = false;
    s_scanned_once = result == ESP_OK;
    lv_obj_remove_state(s_scan_btn, LV_STATE_DISABLED);
    update_network_rows();
    if (result != ESP_OK) {
        set_status("Scan failed. Tap refresh.");
    } else if (count == 0) {
        set_status("No networks found");
    } else if (count > NETWORK_ROWS) {
        char message[64];
        snprintf(message, sizeof(message), "%u networks found", (unsigned)count);
        set_status(message);
    } else {
        set_status("Select a network");
    }
    lvgl_port_unlock();
    vTaskDelete(NULL);
}

static void start_scan(void)
{
    if (s_scanning || s_connecting || s_scan_btn == NULL) return;
    s_scanning = true;
    lv_obj_add_state(s_scan_btn, LV_STATE_DISABLED);
    set_status("Scanning nearby networks...");
    if (xTaskCreate(scan_task, "wifi_scan", 4096, NULL, 5, NULL) != pdPASS) {
        s_scanning = false;
        lv_obj_remove_state(s_scan_btn, LV_STATE_DISABLED);
        set_status("Could not start Wi-Fi scan");
    }
}

static void scan_clicked(lv_event_t *event)
{
    (void)event;
    start_scan();
}

static lv_obj_t *make_button(lv_obj_t *parent, const char *text, int32_t width)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_set_size(button, width, 36);
    lv_obj_set_style_bg_color(button, COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(button, 5, 0);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, COLOR_TEXT, 0);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *make_textarea(lv_obj_t *parent, const char *placeholder, int32_t y)
{
    lv_obj_t *textarea = lv_textarea_create(parent);
    lv_obj_set_size(textarea, 216, 38);
    lv_obj_align(textarea, LV_ALIGN_TOP_MID, 0, y);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_placeholder_text(textarea, placeholder);
    lv_obj_set_style_bg_color(textarea, COLOR_BG_LIGHT, 0);
    lv_obj_set_style_text_color(textarea, COLOR_TEXT, 0);
    lv_obj_set_style_border_color(textarea, lv_color_hex(0x4A4A4A), 0);
    lv_obj_set_style_radius(textarea, 5, 0);
    return textarea;
}

void launcher_ui_init(lv_obj_t *screen)
{
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, COLOR_BG_DARK, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(screen, false);
    launcher_dialogs_init(screen);
    launcher_dialog_set_refresh_callback(ota_manager_refresh);
    file_server_set_transfer_callback(transfer_event);

    lv_obj_t *title_bar = lv_obj_create(screen);
    lv_obj_set_size(title_bar, 240, 34);
    lv_obj_align(title_bar, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_color(title_bar, COLOR_BG_LIGHT, 0);
    lv_obj_set_style_bg_opa(title_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(title_bar, 0, 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_set_style_pad_all(title_bar, 0, 0);
    lv_obj_set_scrollable(title_bar, false);
    lv_obj_set_scrollbar_mode(title_bar, LV_SCROLLBAR_MODE_OFF);

    s_title = lv_label_create(title_bar);
    lv_label_set_text_fmt(s_title, "v%s", esp_app_get_description()->version);
    lv_obj_set_style_text_color(s_title, COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_title, &lv_font_montserrat_14, 0);
    lv_obj_set_width(s_title, 54);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_DOT);
    lv_obj_align(s_title, LV_ALIGN_LEFT_MID, 132, 0);

    lv_obj_t *wifi_button = lv_button_create(title_bar);
    lv_obj_set_size(wifi_button, 40, 30);
    lv_obj_align(wifi_button, LV_ALIGN_LEFT_MID, 2, 0);
    lv_obj_set_style_bg_opa(wifi_button, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(wifi_button, 0, 0);
    lv_obj_add_event_cb(wifi_button, menu_clicked, LV_EVENT_CLICKED, (void *)0);
    s_wifi_icon = lv_label_create(wifi_button);
    lv_label_set_text(s_wifi_icon, LV_SYMBOL_WIFI);
    lv_obj_center(s_wifi_icon);
    s_battery = lv_label_create(title_bar);
    lv_label_set_text(s_battery, LV_SYMBOL_BATTERY_FULL " --%");
    lv_obj_set_style_text_color(s_battery, COLOR_TEXT, 0);
    lv_obj_align(s_battery, LV_ALIGN_LEFT_MID, 46, 0);

    lv_obj_t *nav = lv_button_create(title_bar);
    lv_obj_set_size(nav, 36, 30);
    lv_obj_align(nav, LV_ALIGN_RIGHT_MID, -2, 0);
    lv_obj_set_style_bg_color(nav, COLOR_PRIMARY, 0);
    lv_obj_set_style_radius(nav, 5, 0);
    lv_obj_add_event_cb(nav, nav_clicked, LV_EVENT_CLICKED, NULL);
    s_nav_label = lv_label_create(nav);
    lv_label_set_text(s_nav_label, LV_SYMBOL_LIST);
    lv_obj_set_style_text_color(s_nav_label, COLOR_TEXT, 0);
    lv_obj_center(s_nav_label);

    s_apps_view = lv_obj_create(screen);
    lv_obj_set_size(s_apps_view, 240, 286);
    lv_obj_align(s_apps_view, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_apps_view, COLOR_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_apps_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_apps_view, 0, 0);
    lv_obj_set_style_radius(s_apps_view, 0, 0);
    lv_obj_set_style_pad_all(s_apps_view, 6, 0);
    lv_obj_set_scrollable(s_apps_view, false);
    lv_obj_set_scrollbar_mode(s_apps_view, LV_SCROLLBAR_MODE_OFF);
    ota_manager_init(s_apps_view);

    s_wifi_view = lv_obj_create(screen);
    lv_obj_set_size(s_wifi_view, 240, 286);
    lv_obj_align(s_wifi_view, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_wifi_view, COLOR_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_wifi_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_wifi_view, 0, 0);
    lv_obj_set_style_radius(s_wifi_view, 0, 0);
    lv_obj_set_style_pad_all(s_wifi_view, 6, 0);
    lv_obj_set_scrollable(s_wifi_view, false);
    lv_obj_set_scrollbar_mode(s_wifi_view, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *mode = lv_label_create(s_wifi_view);
    lv_label_set_text(mode, "Nearby networks");
    lv_obj_set_style_text_color(mode, COLOR_MUTED, 0);
    lv_obj_align(mode, LV_ALIGN_TOP_LEFT, 5, 4);

    s_scan_btn = make_button(s_wifi_view, LV_SYMBOL_REFRESH, 34);
    lv_obj_set_height(s_scan_btn, 26);
    lv_obj_align(s_scan_btn, LV_ALIGN_TOP_RIGHT, -5, 0);
    lv_obj_add_event_cb(s_scan_btn, scan_clicked, LV_EVENT_CLICKED, NULL);

    for (size_t i = 0; i < NETWORK_ROWS; ++i) {
        s_network_buttons[i] = lv_button_create(s_wifi_view);
        lv_obj_set_size(s_network_buttons[i], 216, 36);
        lv_obj_align(s_network_buttons[i], LV_ALIGN_TOP_MID, 0, 34 + (int32_t)i * 40);
        lv_obj_set_style_bg_color(s_network_buttons[i], COLOR_BG_LIGHT, 0);
        lv_obj_set_style_radius(s_network_buttons[i], 4, 0);
        lv_obj_set_style_pad_left(s_network_buttons[i], 7, 0);
        lv_obj_add_event_cb(s_network_buttons[i], network_clicked, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        s_network_labels[i] = lv_label_create(s_network_buttons[i]);
        lv_obj_set_width(s_network_labels[i], 200);
        lv_label_set_long_mode(s_network_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(s_network_labels[i], COLOR_TEXT, 0);
        lv_obj_align(s_network_labels[i], LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_hidden(s_network_buttons[i], true);
    }

    /* Password window: field and actions above a fixed keyboard. */
    s_password_popup = lv_obj_create(screen);
    lv_obj_set_size(s_password_popup, 240, 320);
    lv_obj_set_pos(s_password_popup, 0, 0);
    lv_obj_set_style_pad_all(s_password_popup, 0, 0);
    lv_obj_set_style_radius(s_password_popup, 0, 0);
    lv_obj_set_style_border_width(s_password_popup, 0, 0);
    lv_obj_set_style_bg_color(s_password_popup, COLOR_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_password_popup, LV_OPA_COVER, 0);
    lv_obj_set_scrollable(s_password_popup, false);
    lv_obj_t *password_window = lv_obj_create(s_password_popup);
    lv_obj_set_size(password_window, 228, 136);
    lv_obj_set_pos(password_window, 6, 6);
    lv_obj_set_style_bg_color(password_window, COLOR_BG_LIGHT, 0);
    lv_obj_set_style_pad_all(password_window, 0, 0);
    lv_obj_set_style_border_color(password_window, lv_color_hex(0x666666), 0);
    lv_obj_set_scrollable(password_window, false);
    s_password_title = lv_label_create(password_window);
    lv_obj_set_width(s_password_title, 204);
    lv_obj_set_style_text_color(s_password_title, COLOR_TEXT, 0);
    lv_label_set_long_mode(s_password_title, LV_LABEL_LONG_DOT);
    lv_obj_align(s_password_title, LV_ALIGN_TOP_MID, 0, 14);
    s_password = make_textarea(password_window, "Wi-Fi password", 42);
    lv_textarea_set_max_length(s_password, 64);
    lv_textarea_set_password_mode(s_password, true);
    lv_obj_add_event_cb(s_password, textarea_focused, LV_EVENT_FOCUSED, NULL);

    s_connect_btn = make_button(password_window, "Connect", 102);
    lv_obj_align(s_connect_btn, LV_ALIGN_TOP_RIGHT, -6, 92);
    lv_obj_add_event_cb(s_connect_btn, connect_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel = make_button(password_window, "Cancel", 102);
    lv_obj_align(cancel, LV_ALIGN_TOP_LEFT, 6, 92);
    lv_obj_add_event_cb(cancel, password_cancel, LV_EVENT_CLICKED, NULL);

    lv_obj_t *forget = make_button(s_wifi_view, "Forget", 72);
    lv_obj_align(forget, LV_ALIGN_TOP_RIGHT, -6, 200);
    lv_obj_set_style_bg_color(forget, lv_color_hex(0x505050), 0);
    lv_obj_add_event_cb(forget, forget_clicked, LV_EVENT_CLICKED, NULL);
    s_more_networks = make_button(s_wifi_view, "More networks", 132);
    lv_obj_align(s_more_networks, LV_ALIGN_TOP_LEFT, 6, 200);
    lv_obj_add_event_cb(s_more_networks, more_networks, LV_EVENT_CLICKED, NULL);
    lv_obj_set_hidden(s_more_networks, true);

    s_status = lv_label_create(s_wifi_view);
    lv_obj_set_width(s_status, 216);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status, COLOR_TEXT, 0);
    lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, 244);
    set_status(file_server_has_wifi_credentials()
                   ? "Reconnecting saved network..."
                   : "Open Wi-Fi setup to scan");

    s_keyboard = lv_keyboard_create(s_password_popup);
    lv_obj_set_size(s_keyboard, 240, 174);
    lv_obj_align(s_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(s_keyboard, keyboard_event, LV_EVENT_ALL, NULL);
    lv_obj_set_hidden(s_password_popup, true);

    s_menu = lv_obj_create(screen);
    lv_obj_set_size(s_menu, 170, 152);
    lv_obj_align(s_menu, LV_ALIGN_TOP_RIGHT, -4, 36);
    lv_obj_set_style_pad_all(s_menu, 6, 0);
    lv_obj_set_style_bg_color(s_menu, COLOR_BG_LIGHT, 0);
    lv_obj_set_scrollable(s_menu, false);
    const char *items[] = {"Wi-Fi / Apps", "Web files", "Refresh apps"};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *item = make_button(s_menu, items[i], 152);
        lv_obj_set_pos(item, 0, i * 44);
        lv_obj_add_event_cb(item, menu_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    lv_obj_set_hidden(s_menu, true);
    lv_timer_create(status_tick, 3000, NULL);

    show_wifi(false);
    if (!sd_is_mounted()) {
        launcher_dialog_message("No SD card detected", "Apps and file transfers need an SD card. Insert it and restart.");
    }

    if (file_server_has_wifi_credentials()) {
        s_connecting = true;
        lv_obj_add_state(s_connect_btn, LV_STATE_DISABLED);
        if (xTaskCreate(connection_task, "wifi_reconnect", 6144, NULL, 5, NULL) != pdPASS) {
            s_connecting = false;
            lv_obj_remove_state(s_connect_btn, LV_STATE_DISABLED);
            set_status("Could not start reconnect task");
        }
    }
}
