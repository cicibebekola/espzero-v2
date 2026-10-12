idf_component_register(
    SRCS "pulldown_bar.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        lvgl
        esp_lvgl_port
        esp_driver_gpio
        lvgl_runtime
        sleep_manager
        sd_card
        freertos
)
