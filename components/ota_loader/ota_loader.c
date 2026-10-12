/**
 * ota_loader.c
 *
 * Implementation of ota_loader_flash_sd().
 */
#include "ota_loader.h"
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs.h"

static const char *TAG = "ota_loader";

#define OTA_BUF_SIZE 4096  // 4KB buffer, low RAM

esp_err_t ota_loader_select_launcher(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *launcher = NULL;
    uint32_t address = 0;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("app_boot", NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_u32(handle, "launcher_addr", &address);
        nvs_close(handle);
        if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) return err;
    } else if (err != ESP_ERR_NVS_NOT_FOUND) return err;
    if (address) {
        esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
        while (it) {
            const esp_partition_t *part = esp_partition_get(it);
            if (part->address == address) { launcher = part; esp_partition_iterator_release(it); break; }
            it = esp_partition_next(it);
        }
    } else {
        /* Existing devices keep Launcher in ota_0 and install apps in ota_1. */
        launcher = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    }
    if (!running || !launcher || launcher->address == running->address) return ESP_ERR_INVALID_STATE;
    esp_app_desc_t description;
    err = esp_ota_get_partition_description(launcher, &description);
    if (err != ESP_OK) return err;
    if (strcmp(description.project_name, "esp_zero") || !strncmp(description.version, "music-", 6))
        return ESP_ERR_INVALID_STATE;
    ESP_LOGI(TAG, "Return to launcher: %s at 0x%08lx", launcher->label, (unsigned long)launcher->address);
    /* This also verifies the complete target image before selecting it. */
    return esp_ota_set_boot_partition(launcher);
}

static esp_err_t remember_launcher(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return ESP_ERR_INVALID_STATE;
    nvs_handle_t handle;
    esp_err_t err = nvs_open("app_boot", NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(handle, "launcher_addr", running->address);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t ota_loader_flash_sd(const char *sd_path,
                             ota_progress_cb_t progress_cb,
                             void *cb_arg,
                             bool reboot_on_success)
{
    if (!sd_path) {
        ESP_LOGE(TAG, "sd_path is NULL");
        return ESP_ERR_INVALID_ARG;
    }

    struct stat st;
    size_t file_size = 0;
    if (stat(sd_path, &st) == 0) {
        file_size = (size_t)st.st_size;
        ESP_LOGI(TAG, "File size: %zu bytes", file_size);
    } else {
        ESP_LOGW(TAG, "Could not stat '%s' (will try to stream anyway)", sd_path);
    }

    // Find target partition (next update partition)
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        ESP_LOGE(TAG, "No update partition available");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Flashing to partition subtype %d at offset 0x%08x (size 0x%08x)",
             update_part->subtype, update_part->address, update_part->size);

    if (file_size > 0 && (size_t)update_part->size < file_size) {
        ESP_LOGE(TAG, "Image (%u bytes) is larger than target partition (%u bytes)",
                 (unsigned)file_size, (unsigned)update_part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(sd_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", sd_path);
        return ESP_ERR_NOT_FOUND;
    }

    esp_ota_handle_t ota_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        fclose(f);
        return err;
    }

    uint8_t *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        ESP_LOGE(TAG, "No memory for OTA buffer");
        esp_ota_abort(ota_handle);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t total_written = 0;
    size_t read_bytes = 0;
    
    // Call progress callback every 4KB (every buffer) for smoother updates
    const size_t progress_step = OTA_BUF_SIZE;
    size_t next_progress = progress_step;
    
    // Initial progress callback (0%)
    if (progress_cb) {
        progress_cb(0, file_size, cb_arg);
    }

    while ((read_bytes = fread(buf, 1, OTA_BUF_SIZE, f)) > 0) {
        err = esp_ota_write(ota_handle, buf, read_bytes);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            free(buf);
            esp_ota_abort(ota_handle);
            fclose(f);
            return err;
        }

        total_written += read_bytes;

        // Call progress callback more frequently - every 4KB or at completion
        if (progress_cb && (total_written >= next_progress || (file_size > 0 && total_written == file_size))) {
            progress_cb(total_written, file_size, cb_arg);
            next_progress = total_written + progress_step;
        }
    }

    free(buf);
    fclose(f);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        esp_ota_abort(ota_handle);
        return err;
    }

    // Optionally verify image (esp_image_verify, signature check) here

    err = remember_launcher();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Could not save launcher return slot: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "OTA write complete (%u bytes). Boot partition set.", (unsigned)total_written);

    // Final progress callback (100%)
    if (progress_cb) {
        progress_cb(total_written, file_size, cb_arg);
    }

    if (reboot_on_success) {
        ESP_LOGI(TAG, "Rebooting to new partition...");
        esp_restart();
        // not reached
    }

    return ESP_OK;
}
