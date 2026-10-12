#include "file_server.h"
#include "file_manager.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>
#include <sys/stat.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "sleep_manager.h"

static const char *TAG = "file_server";
static file_server_transfer_cb_t s_transfer_callback;
static portMUX_TYPE s_storage_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_storage_busy;

bool file_server_acquire_storage(void)
{
    portENTER_CRITICAL(&s_storage_mux);
    bool acquired = !s_storage_busy;
    if (acquired) s_storage_busy = true;
    portEXIT_CRITICAL(&s_storage_mux);
    return acquired;
}

void file_server_release_storage(void)
{
    portENTER_CRITICAL(&s_storage_mux);
    s_storage_busy = false;
    portEXIT_CRITICAL(&s_storage_mux);
}

void file_server_set_transfer_callback(file_server_transfer_cb_t callback)
{
    s_transfer_callback = callback;
}

static void transfer_event(const char *operation, const char *filename,
                           size_t done, size_t total, bool finished, bool success)
{
    if (s_transfer_callback != NULL) {
        s_transfer_callback(operation, filename, done, total, finished, success);
    }
}

static httpd_handle_t s_server = NULL;
static char s_ip_str[16] = {0};
static char s_mount_point[64] = "/sdcard";
static bool s_running = false;
static bool s_wifi_initialized = false;
static bool s_wifi_started = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
static esp_event_handler_instance_t s_wifi_handler;
static esp_event_handler_instance_t s_ip_handler;

#define AP_SSID "EspZero"
#define AP_PASS "12345678"


/* ── WiFi ── */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_count = 0;

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_count < 5) { esp_wifi_connect(); s_retry_count++; }
        else xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&ev->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static esp_err_t ensure_wifi_initialized(void)
{
    if (s_wifi_initialized) return ESP_OK;

    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;

    s_wifi_event_group = xEventGroupCreate();
    if (s_wifi_event_group == NULL) return ESP_ERR_NO_MEM;

    s_sta_netif = esp_netif_create_default_wifi_sta();
    if (s_sta_netif == NULL) return ESP_FAIL;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) return err;

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL,
                                               &s_wifi_handler);
    if (err != ESP_OK) return err;
    err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL,
                                               &s_ip_handler);
    if (err != ESP_OK) return err;

    s_wifi_initialized = true;
    return ESP_OK;
}

static esp_err_t start_wifi(void)
{
    esp_err_t err = ensure_wifi_initialized();
    if (err != ESP_OK) return err;

    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    s_retry_count = 0;

    /* Try saved creds */
    wifi_config_t wifi_cfg = {0};
    char saved_ssid[33] = {0};
    char saved_password[65] = {0};
    if (file_server_load_wifi_credentials(saved_ssid, saved_password) == ESP_OK) {
        memcpy(wifi_cfg.sta.ssid, saved_ssid, strlen(saved_ssid));
        memcpy(wifi_cfg.sta.password, saved_password, strlen(saved_password));
    }

    if (wifi_cfg.sta.ssid[0]) {
        ESP_LOGI(TAG, "WiFi STA: connecting to %s", saved_ssid);
        if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
        if ((err = esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg)) != ESP_OK) return err;
        if ((err = esp_wifi_start()) != ESP_OK) return err;
        s_wifi_started = true;

        EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000));
        if (bits & WIFI_CONNECTED_BIT) return ESP_OK;

        ESP_LOGW(TAG, "STA failed, falling back to AP");
        esp_wifi_stop();
        s_wifi_started = false;
    }

    /* AP mode */
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) return ESP_FAIL;
    }
    wifi_config_t ap_cfg = {
        .ap = { .ssid = AP_SSID, .ssid_len = strlen(AP_SSID),
                .password = AP_PASS, .max_connection = 4,
                .authmode = WIFI_AUTH_WPA2_PSK },
    };
    if ((err = esp_wifi_set_mode(WIFI_MODE_AP)) != ESP_OK) return err;
    if ((err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg)) != ESP_OK) return err;
    if ((err = esp_wifi_start()) != ESP_OK) return err;
    s_wifi_started = true;
    snprintf(s_ip_str, sizeof(s_ip_str), "192.168.4.1");
    ESP_LOGI(TAG, "AP mode: %s (pass: %s)", AP_SSID, AP_PASS);
    return ESP_OK;
}

esp_err_t file_server_scan_wifi(file_server_wifi_network_t *networks,
                                size_t capacity, size_t *count)
{
    if (networks == NULL || count == NULL || capacity == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    *count = 0;

    esp_err_t err = ensure_wifi_initialized();
    if (err != ESP_OK) return err;

    wifi_mode_t mode = WIFI_MODE_NULL;
    esp_wifi_get_mode(&mode);
    if (mode == WIFI_MODE_AP) {
        if ((err = esp_wifi_set_mode(WIFI_MODE_APSTA)) != ESP_OK) return err;
    } else if (mode == WIFI_MODE_NULL) {
        if ((err = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK) return err;
    }

    if (!s_wifi_started) {
        if ((err = esp_wifi_start()) != ESP_OK) return err;
        s_wifi_started = true;
    }

    wifi_scan_config_t scan = {
        .ssid = NULL,
        .bssid = NULL,
        .channel = 0,
        .show_hidden = false,
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    err = esp_wifi_scan_start(&scan, true);
    if (err != ESP_OK) return err;

    wifi_ap_record_t records[20];
    uint16_t record_count = capacity < 20 ? (uint16_t)capacity : 20;
    err = esp_wifi_scan_get_ap_records(&record_count, records);
    if (err != ESP_OK) return err;

    for (uint16_t i = 0; i < record_count && *count < capacity; ++i) {
        if (records[i].ssid[0] == '\0') continue;

        bool duplicate = false;
        for (size_t j = 0; j < *count; ++j) {
            if (strcmp(networks[j].ssid, (const char *)records[i].ssid) == 0) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) continue;

        snprintf(networks[*count].ssid, sizeof(networks[*count].ssid), "%s",
                 (const char *)records[i].ssid);
        networks[*count].rssi = records[i].rssi;
        networks[*count].secure = records[i].authmode != WIFI_AUTH_OPEN;
        (*count)++;
    }
    return ESP_OK;
}

/* ── Public API ── */

esp_err_t file_server_start(const char *mount_point)
{
    if (s_running) return ESP_OK;
    strncpy(s_mount_point, mount_point, sizeof(s_mount_point) - 1);

    esp_err_t err = start_wifi();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi start failed: %s", esp_err_to_name(err));
        return err;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    /* HTTP -> path validation -> FatFs -> SD/SPI is a deep call chain. */
    config.stack_size = 12 * 1024;
    config.uri_match_fn = httpd_uri_match_wildcard;
    err = httpd_start(&s_server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(err));
        return err;
    }

    err = file_manager_register(s_server, s_mount_point, transfer_event);
    if (err != ESP_OK) {
        httpd_stop(s_server);
        s_server = NULL;
        return err;
    }

    s_running = true;
    sleep_manager_set_inhibited(true);
    ESP_LOGI(TAG, "File server: http://%s/", s_ip_str);
    return ESP_OK;
}

void file_server_stop(void)
{
    if (s_server) { httpd_stop(s_server); s_server = NULL; }
    if (s_wifi_started) {
        esp_wifi_stop();
        s_wifi_started = false;
    }
    s_running = false;
    sleep_manager_set_inhibited(false);
}

bool file_server_is_running(void) { return s_running; }
const char *file_server_get_ip(void) { return s_ip_str; }
