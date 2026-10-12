idf_component_register(
    SRCS "main.c" "launcher_ui.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        lvgl
        lvgl_runtime
        esp_lvgl_port
        touch_xpt2046
        ota_manager
        launcher_dialogs
        esp_app_format
        file_server
        esp_wifi
        esp_event
        esp_netif
        sd_card
        sleep_manager
        esp_timer
        esp_driver_gpio
        nvs_flash
        freertos
)
