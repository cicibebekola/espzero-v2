idf_component_register(
    SRCS "tp_spi.c" "xpt2046.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        lvgl
        esp_lvgl_port
        esp_driver_spi
        esp_driver_gpio
        device_config
        freertos
)
