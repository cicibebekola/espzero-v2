#pragma once
/**
 * file_server.h — Simple HTTP file upload/download server for SD card
 *
 * Starts WiFi (STA if creds saved, else AP mode) + HTTP server.
 * Browse to http://<ip>/ to upload/download/delete files.
 */
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start WiFi + HTTP file server.
 * @param mount_point  SD card mount point (e.g. "/sdcard")
 * @return ESP_OK on success
 */
esp_err_t file_server_start(const char *mount_point);
esp_err_t file_server_save_wifi_credentials(const char *ssid, const char *password);
esp_err_t file_server_clear_wifi_credentials(void);
bool file_server_has_wifi_credentials(void);
/* Call after mounting SD and initializing NVS, before building the Wi-Fi UI. */
void file_server_restore_wifi_credentials(void);
esp_err_t file_server_load_wifi_credentials(char ssid[33], char password[65]);

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure;
} file_server_wifi_network_t;

/**
 * @brief Scan for nearby Wi-Fi networks. Results are ordered by signal strength.
 */
esp_err_t file_server_scan_wifi(file_server_wifi_network_t *networks,
                                size_t capacity, size_t *count);

/**
 * @brief Stop the file server and WiFi.
 */
void file_server_stop(void);

/**
 * @brief Check if server is running.
 */
bool file_server_is_running(void);

/**
 * @brief Get the server IP address string (e.g. "192.168.1.100")
 */
const char *file_server_get_ip(void);

typedef void (*file_server_transfer_cb_t)(const char *operation, const char *filename,
                                         size_t done, size_t total, bool finished,
                                         bool success);
/* Callback executes on the HTTP task; enqueue UI work rather than calling LVGL. */
void file_server_set_transfer_callback(file_server_transfer_cb_t callback);
/* Serialize HTTP file operations with firmware reads from SD. */
bool file_server_acquire_storage(void);
void file_server_release_storage(void);

#ifdef __cplusplus
}
#endif
