idf_component_register(
    SRCS "ota_loader.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        app_update
        esp_partition
        esp_system
        freertos
        nvs_flash
)
