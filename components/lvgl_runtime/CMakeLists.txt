idf_component_register(
    SRCS "lvgl_runtime.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_timer
    PRIV_REQUIRES
        esp_lcd
        esp_lvgl_port
        esp_driver_spi
        esp_driver_gpio
        esp_driver_ledc
        freertos
        device_config
)
if(ACTIVE_APP STREQUAL "bt_music")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE ESPZERO_MUSIC_PROFILE=1)
endif()
