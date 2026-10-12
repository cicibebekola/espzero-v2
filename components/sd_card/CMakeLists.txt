idf_component_register(
    SRCS "sd_card_manager.c"
    INCLUDE_DIRS "."
    REQUIRES
        device_config
    PRIV_REQUIRES
        driver
        fatfs
        freertos
        sdmmc
        esp_driver_spi
)
