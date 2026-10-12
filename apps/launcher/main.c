/*
 * Launcher — minimal main screen with pull-down bar + OTA app manager
 */

#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "lvgl_runtime.h"
#include "sd_card_manager.h"
#include "sleep_manager.h"
#include "board_config.h"
#include "device_paths.h"
#include "ota_manager.h"
#include "xpt2046.h"
#include "file_server.h"
#include "launcher_ui.h"

#define TAG "LAUNCHER"

#define COL_BG        lv_color_hex(0x1a1a2e)
#define COL_CARD      lv_color_hex(0x16213e)
#define COL_TEXT      lv_color_hex(0xeaeaea)
#define COL_TEXT_DIM  lv_color_hex(0x8899aa)

/* ──────────────────────────────────────────────
 * Battery timer (updates pull-down bar)
 * ────────────────────────────────────────────── */

#if 0 /* Legacy tab UI retained temporarily for reference; launcher_ui.c owns UI. */

/* UI lives in launcher_ui.c; retained legacy helpers are unreachable until removed. */
static lv_obj_t *s_ssid, *s_password, *s_wifi_status, *s_server_status;
static void wifi_save_cb(lv_event_t *e)
{
    (void)e;
    const char *ssid = lv_textarea_get_text(s_ssid);
    const char *pass = lv_textarea_get_text(s_password);
    esp_err_t err = file_server_save_wifi_credentials(ssid, pass);
    if (err == ESP_OK) lv_label_set_text(s_wifi_status, "Saved. Reboot to connect.");
    else lv_label_set_text_fmt(s_wifi_status, "Save failed: %s", esp_err_to_name(err));
}
static void wifi_clear_cb(lv_event_t *e)
{
    (void)e;
    file_server_clear_wifi_credentials();
    lv_textarea_set_text(s_ssid, ""); lv_textarea_set_text(s_password, "");
    lv_label_set_text(s_wifi_status, "Saved network removed.");
}
static void server_start_cb(lv_event_t *e)
{
    (void)e;
    if (!sd_is_mounted()) { lv_label_set_text(s_server_status, "Insert an SD card first."); return; }
    esp_err_t err = file_server_start(DEVICE_SD_MOUNT_POINT);
    if (err == ESP_OK)
        lv_label_set_text_fmt(s_server_status, "File manager: http://%s/", file_server_get_ip());
    else
        lv_label_set_text_fmt(s_server_status, "Server failed: %s", esp_err_to_name(err));
}

/* ──────────────────────────────────────────────
 * UI builder (called with lvgl_port_lock held)
 * ────────────────────────────────────────────── */

static void build_ui(void)
{
    launcher_ui_init(lv_screen_active());
    return;
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *tabs = lv_tabview_create(scr);
    lv_obj_set_size(tabs, 240, 308); lv_obj_set_pos(tabs, 0, 12);
    lv_tabview_set_tab_bar_position(tabs, LV_DIR_TOP);
    lv_obj_t *wifi = lv_tabview_add_tab(tabs, "Wi-Fi");
    lv_obj_t *files = lv_tabview_add_tab(tabs, "Files");
    lv_obj_t *ota = lv_tabview_add_tab(tabs, "Apps");
    lv_obj_t *title = lv_label_create(wifi); lv_label_set_text(title, "Wi-Fi setup"); lv_obj_set_pos(title, 8, 8);
    s_ssid = lv_textarea_create(wifi); lv_textarea_set_placeholder_text(s_ssid, "Network name"); lv_obj_set_size(s_ssid, 210, 38); lv_obj_set_pos(s_ssid, 8, 35);
    s_password = lv_textarea_create(wifi); lv_textarea_set_placeholder_text(s_password, "Password"); lv_textarea_set_password_mode(s_password, true); lv_obj_set_size(s_password, 210, 38); lv_obj_set_pos(s_password, 8, 80);
    lv_obj_t *save = lv_btn_create(wifi); lv_obj_set_pos(save, 8, 130); lv_obj_set_size(save, 100, 35); lv_obj_add_event_cb(save, wifi_save_cb, LV_EVENT_CLICKED, NULL); lv_label_set_text(lv_label_create(save), "Save");
    lv_obj_t *clear = lv_btn_create(wifi); lv_obj_set_pos(clear, 118, 130); lv_obj_set_size(clear, 100, 35); lv_obj_add_event_cb(clear, wifi_clear_cb, LV_EVENT_CLICKED, NULL); lv_label_set_text(lv_label_create(clear), "Forget");
    s_wifi_status = lv_label_create(wifi); lv_label_set_text(s_wifi_status, file_server_has_wifi_credentials() ? "Saved network reconnects after reboot." : "Enter a network to enable STA mode."); lv_obj_set_width(s_wifi_status, 210); lv_obj_set_pos(s_wifi_status, 8, 180);
    lv_obj_t *start = lv_btn_create(files); lv_obj_set_pos(start, 8, 20); lv_obj_set_size(start, 210, 38); lv_obj_add_event_cb(start, server_start_cb, LV_EVENT_CLICKED, NULL); lv_label_set_text(lv_label_create(start), "Start SD file manager");
    s_server_status = lv_label_create(files); lv_label_set_text(s_server_status, "Upload, download, or delete SD files over Wi-Fi."); lv_obj_set_width(s_server_status, 210); lv_obj_set_pos(s_server_status, 8, 75);
    ota_manager_init(ota);
}

#endif

/* ──────────────────────────────────────────────
 * app_main
 * ────────────────────────────────────────────── */

void app_main(void)
{
    ESP_LOGI(TAG, "=== EspZero v2 — launcher ===");

    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* SD card */
    ret = sd_init();
    ESP_LOGI(TAG, "SD card: %s", ret == ESP_OK ? "OK" : esp_err_to_name(ret));
    if (ret == ESP_OK) file_server_restore_wifi_credentials();

    /* LVGL 9 + display */
    esp_timer_handle_t tick_timer = NULL;
    ESP_ERROR_CHECK(lvgl_runtime_init(2, 2, &tick_timer));

    /* Sleep manager */
    sleep_manager_init(tick_timer);

    /* LEDs off */
    gpio_set_direction(BOARD_LED_RED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOARD_LED_GREEN_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_direction(BOARD_LED_BLUE_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(BOARD_LED_RED_GPIO, 1);
    gpio_set_level(BOARD_LED_GREEN_GPIO, 1);
    gpio_set_level(BOARD_LED_BLUE_GPIO, 1);

    /* Touch */
    xpt2046_init();

    /* Build UI with LVGL lock */
    lvgl_port_lock(portMAX_DELAY);

    /* Main UI */
    launcher_ui_init(lv_screen_active());

    lvgl_port_unlock();

    ESP_LOGI(TAG, "Init done — ready");

    /* Idle loop */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
