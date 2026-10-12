idf_component_register(
    SRCS "file_server.c" "wifi_credentials.c" "file_manager.c"
    INCLUDE_DIRS "include"
    EMBED_TXTFILES "web/index.html"
    PRIV_REQUIRES
        nvs_flash
        esp_wifi
        esp_event
        esp_netif
        esp_http_server
        freertos
        sleep_manager
        device_config
)
target_compile_options(${COMPONENT_LIB} PRIVATE -fstack-usage)
