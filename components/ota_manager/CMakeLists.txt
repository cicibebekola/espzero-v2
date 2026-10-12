idf_component_register(
    SRCS "ota_manager.c"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES
        lvgl
        esp_lvgl_port
        sd_card
        ota_loader
        device_config
        freertos
        launcher_dialogs
        esp_system
        file_server
)
