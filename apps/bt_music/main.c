#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "lvgl_runtime.h"
#include "sd_card_manager.h"
#include "sleep_manager.h"
#include "board_config.h"
#include "xpt2046.h"

#include "bt_music_backend.h"
#include "mp3_player.h"
#include "bt_music_ui.h"

#define TAG "BT_MUSIC"

void app_main(void)
{
    /* NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* SD card — music at /sdcard/music/ */
    if (sd_init() != ESP_OK) {
        ESP_LOGE(TAG, "SD init failed — continuing without music");
    }

    /* Bluetooth */
    if (bt_music_init() != ESP_OK) {
        ESP_LOGE(TAG, "BT init failed");
    }
    ESP_LOGI(TAG, "Free heap after BT: %u", heap_caps_get_free_size(MALLOC_CAP_DEFAULT));

    /* MP3 player */
    ret = mp3_player_init(NULL, NULL);
    if (ret != ESP_OK) ESP_LOGE(TAG, "Music player init failed: %s", esp_err_to_name(ret));
    ret = mp3_player_scan();
    if (ret != ESP_OK) ESP_LOGW(TAG, "Music scan failed: %s", esp_err_to_name(ret));

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

    /* Build UI */
    lvgl_port_lock(portMAX_DELAY);
    bt_music_ui_init();
    lvgl_port_unlock();

    ESP_LOGI(TAG, "BT Music ready");
    ESP_LOGI(TAG, "Heap after UI: free=%u, largest=%u", heap_caps_get_free_size(MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));

    /* Idle */
    bool sleep_locked = false;
    while (1) {
        bt_conn_state_t bt_state = bt_music_get_state();
        bool inhibit = bt_state == BT_STATE_CONNECTED || bt_state == BT_STATE_CONNECTING ||
                       bt_state == BT_STATE_SCANNING || mp3_player_get_state() == MP3_STATE_PLAYING;
        if (inhibit != sleep_locked) {
            sleep_manager_set_inhibited(inhibit);
            sleep_locked = inhibit;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
