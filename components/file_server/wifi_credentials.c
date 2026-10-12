#include "file_server.h"
#include "device_paths.h"
#include "esp_log.h"
#include "nvs.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/* Debug backup contains the password in plaintext. Keep it off HTTP routes. */
#define BACKUP_PATH DEVICE_SD_MOUNT_POINT "/.wifi-creds.bin"
static const char *TAG = "wifi_creds";

typedef struct {
    char magic[8];
    char ssid[33];
    char password[65];
} credential_backup_t;

static bool valid_credentials(const credential_backup_t *value)
{
    return value->ssid[0] != '\0' &&
           memchr(value->ssid, '\0', sizeof(value->ssid)) != NULL &&
           memchr(value->password, '\0', sizeof(value->password)) != NULL;
}

static esp_err_t read_nvs(credential_backup_t *value)
{
    memset(value, 0, sizeof(*value));
    memcpy(value->magic, "EZWiFi1", 8);
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_creds", NVS_READONLY, &handle);
    if (err != ESP_OK) return err;
    size_t ssid_size = sizeof(value->ssid);
    size_t password_size = sizeof(value->password);
    err = nvs_get_str(handle, "ssid", value->ssid, &ssid_size);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, "pass", value->password, &password_size);
    }
    nvs_close(handle);
    if (err == ESP_OK && !valid_credentials(value)) return ESP_ERR_INVALID_ARG;
    return err;
}

static esp_err_t write_nvs(const credential_backup_t *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_creds", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_str(handle, "ssid", value->ssid);
    if (err == ESP_OK) err = nvs_set_str(handle, "pass", value->password);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static bool read_backup(credential_backup_t *value)
{
    FILE *file = fopen(BACKUP_PATH, "rb");
    if (file == NULL) return false;
    bool valid = fread(value, 1, sizeof(*value), file) == sizeof(*value);
    valid = valid && fgetc(file) == EOF;
    fclose(file);
    return valid && memcmp(value->magic, "EZWiFi1", 8) == 0 && valid_credentials(value);
}

static void write_backup(const credential_backup_t *value)
{
    credential_backup_t previous;
    if (read_backup(&previous) && memcmp(&previous, value, sizeof(previous)) == 0) return;

    FILE *file = fopen(BACKUP_PATH, "wb");
    if (file == NULL) {
        ESP_LOGW(TAG, "NVS saved; SD backup unavailable (%s)", strerror(errno));
        return;
    }
    bool written = fwrite(value, 1, sizeof(*value), file) == sizeof(*value);
    if (fclose(file) != 0) written = false;
    if (written) ESP_LOGI(TAG, "Wi-Fi credentials backed up to SD");
    else ESP_LOGW(TAG, "SD credential backup write failed");
}

void file_server_restore_wifi_credentials(void)
{
    credential_backup_t value;
    if (read_nvs(&value) == ESP_OK) {
        /* Seed the backup from existing credentials on the first debug boot. */
        write_backup(&value);
        return;
    }
    if (read_backup(&value)) {
        esp_err_t err = write_nvs(&value);
        if (err == ESP_OK) ESP_LOGI(TAG, "Wi-Fi credentials restored from SD to NVS");
        else ESP_LOGW(TAG, "SD restore failed: %s", esp_err_to_name(err));
    }
}

esp_err_t file_server_save_wifi_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL || ssid[0] == '\0' ||
        strlen(ssid) > 32 || strlen(password) > 64) return ESP_ERR_INVALID_ARG;
    credential_backup_t value = {0};
    memcpy(value.magic, "EZWiFi1", 8);
    memcpy(value.ssid, ssid, strlen(ssid));
    memcpy(value.password, password, strlen(password));
    esp_err_t err = write_nvs(&value);
    if (err == ESP_OK) write_backup(&value);
    return err;
}

esp_err_t file_server_clear_wifi_credentials(void)
{
    /* Remove the backup first, so Forget cannot resurrect it on reboot. */
    if (remove(BACKUP_PATH) != 0 && errno != ENOENT) {
        ESP_LOGW(TAG, "Could not remove SD credential backup (%s)", strerror(errno));
        return ESP_FAIL;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open("wifi_creds", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_erase_all(handle);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

bool file_server_has_wifi_credentials(void)
{
    credential_backup_t value;
    return read_nvs(&value) == ESP_OK;
}

esp_err_t file_server_load_wifi_credentials(char ssid[33], char password[65])
{
    credential_backup_t value;
    esp_err_t err = read_nvs(&value);
    if (err == ESP_OK) {
        memcpy(ssid, value.ssid, sizeof(value.ssid));
        memcpy(password, value.password, sizeof(value.password));
    }
    return err;
}
