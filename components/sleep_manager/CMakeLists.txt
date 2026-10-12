idf_component_register(
    SRCS "sleep_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        device_config
        esp_timer
    PRIV_REQUIRES
        driver
        esp_driver_gpio
        freertos
        esp_adc
        esp_rom
)
