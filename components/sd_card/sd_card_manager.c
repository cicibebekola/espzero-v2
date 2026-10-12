#include "sd_card_manager.h"
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <dirent.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "driver/spi_master.h"
#include "driver/sdspi_host.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

static const char *TAG = "sd_card_manager";

static sdmmc_card_t *s_card = NULL;
static bool s_card_mounted = false;

typedef enum {
    SD_IO_WRITE,
    SD_IO_APPEND,
    SD_IO_EXIT
} sd_io_type_t;

typedef struct {
    sd_io_type_t type;
    char path[128];
    uint8_t *data;
    size_t length;
    SemaphoreHandle_t done;
    esp_err_t result;
} sd_io_msg_t;

static QueueHandle_t s_io_queue = NULL;
static TaskHandle_t s_io_task = NULL;

#define MAX_CACHED_FILES 50
static char *s_file_array[MAX_CACHED_FILES];
static int s_file_count = 0;

static esp_err_t sd_check_mounted(void);
static void sd_io_task(void *arg);
static void sd_free_file_array(void);

esp_err_t sd_init(void)
{
    return sd_card_init(false);
}

esp_err_t sd_card_init(bool format_if_failed)
{
    if (s_card_mounted) {
        ESP_LOGW(TAG, "SD already mounted");
        return ESP_OK;
    }

    esp_err_t ret;

    ESP_LOGI(TAG, "Initializing SD card...");

    spi_bus_free(SD_SPI_HOST);
    vTaskDelay(pdMS_TO_TICKS(150));

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = format_if_failed,
        .max_files = SD_MAX_OPEN_FILES,
        .allocation_unit_size = SD_ALLOCATION_UNIT_SIZE
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = SD_SPI_FREQ_KHZ;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = SD_PIN_MOSI,
        .miso_io_num = SD_PIN_MISO,
        .sclk_io_num = SD_PIN_CLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = SD_MAX_TRANSFER_SIZE
    };

    ret = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);

    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ESP_ERR_SD_MOUNT_FAILED;
    }

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SD_PIN_CS;
    slot_config.host_id = SD_SPI_HOST;

    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config, &mount_config, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "mount failed: %s", esp_err_to_name(ret));
        spi_bus_free(SD_SPI_HOST);
        return ESP_ERR_SD_MOUNT_FAILED;
    }

    s_card_mounted = true;
    sdmmc_card_print_info(stdout, s_card);

    ESP_LOGI(TAG, "SD mounted OK");

    if (!s_io_queue) {
        s_io_queue = xQueueCreate(10, sizeof(sd_io_msg_t));
        if (s_io_queue)
            xTaskCreatePinnedToCore(sd_io_task, "sd_io_task", 4096, NULL, 5, &s_io_task, 0);
    }

    return ESP_OK;
}

esp_err_t sd_card_deinit(void)
{
    if (!s_card_mounted) return ESP_OK;

    if (s_io_queue) {
        sd_io_msg_t msg = {.type = SD_IO_EXIT};
        xQueueSend(s_io_queue, &msg, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(100));

        vQueueDelete(s_io_queue);
        s_io_queue = NULL;
    }

    sd_free_file_array();

    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    spi_bus_free(SD_SPI_HOST);

    s_card = NULL;
    s_card_mounted = false;

    ESP_LOGI(TAG, "SD unmounted");
    return ESP_OK;
}

int sd_list_files(const char *path)
{
    if (!s_card_mounted) {
        ESP_LOGE(TAG, "SD card not mounted");
        return -1;
    }

    sd_free_file_array();
    s_file_count = 0;

    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGE(TAG, "Failed to open directory: %s", path);
        return -1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) && s_file_count < MAX_CACHED_FILES) {
        if (entry->d_name[0] == '.') continue;

        size_t len = strlen(entry->d_name);
        if (len < 4 || strcmp(&entry->d_name[len - 4], ".bin") != 0) continue;

        char full_path[300];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);

        s_file_array[s_file_count] = malloc(strlen(full_path) + 1);
        if (s_file_array[s_file_count]) {
            strcpy(s_file_array[s_file_count], full_path);
            s_file_count++;
            ESP_LOGI(TAG, "Found file: %s", full_path);
        }
    }

    closedir(dir);

    ESP_LOGI(TAG, "Found %d .bin files", s_file_count);
    return s_file_count;
}

char **sd_get_file_array(void)
{
    return s_file_array;
}

static void sd_free_file_array(void)
{
    for (int i = 0; i < s_file_count; i++) {
        if (s_file_array[i]) {
            free(s_file_array[i]);
            s_file_array[i] = NULL;
        }
    }
    s_file_count = 0;
}

esp_err_t sd_list_files_struct(const char *path, sd_file_t *files, size_t max_files, size_t *out_count)
{
    if (!files || max_files == 0 || !out_count) return ESP_ERR_INVALID_ARG;
    *out_count = 0;

    DIR *dir = opendir(path);
    if (!dir) return ESP_FAIL;

    struct dirent *entry;
    while ((entry = readdir(dir)) && *out_count < max_files) {
        strncpy(files[*out_count].name, entry->d_name, sizeof(files[*out_count].name)-1);
        files[*out_count].name[sizeof(files[*out_count].name)-1] = 0;
        files[*out_count].is_dir = (entry->d_type == DT_DIR);
        (*out_count)++;
    }

    closedir(dir);
    return ESP_OK;
}

esp_err_t sd_write_file(const char *path, const char *data)
{
    if (sd_check_mounted() != ESP_OK) return ESP_ERR_SD_NOT_MOUNTED;

    FILE *f = fopen(path, "w");
    if (!f) return ESP_ERR_SD_FILE_FAILED;

    fwrite(data, 1, strlen(data), f);
    fclose(f);
    return ESP_OK;
}

esp_err_t sd_append_file(const char *path, const char *data)
{
    if (sd_check_mounted() != ESP_OK) return ESP_ERR_SD_NOT_MOUNTED;

    FILE *f = fopen(path, "a");
    if (!f) return ESP_ERR_SD_FILE_FAILED;

    fwrite(data, 1, strlen(data), f);
    fclose(f);
    return ESP_OK;
}

esp_err_t sd_read_file(const char *path, char *buffer, size_t buffer_size)
{
    if (!buffer || buffer_size == 0) return ESP_ERR_INVALID_ARG;
    if (sd_check_mounted() != ESP_OK) return ESP_ERR_SD_NOT_MOUNTED;

    FILE *f = fopen(path, "r");
    if (!f) return ESP_ERR_SD_FILE_FAILED;

    size_t read_len = fread(buffer, 1, buffer_size - 1, f);

    if (read_len == 0 && !feof(f)) {
        fclose(f);
        return ESP_ERR_SD_FILE_FAILED;
    }

    buffer[read_len] = '\0';

    if (read_len > 0 && (buffer[read_len - 1] == '\n' || buffer[read_len - 1] == '\r')) {
        buffer[read_len - 1] = '\0';
    }

    fclose(f);
    return ESP_OK;
}

esp_err_t sd_queue_write(const char *path, const uint8_t *data, size_t len, bool wait)
{
    if (!s_card_mounted || !s_io_queue) return ESP_ERR_SD_NOT_MOUNTED;

    sd_io_msg_t msg = {0};
    msg.type = SD_IO_WRITE;
    strncpy(msg.path, path, sizeof(msg.path) - 1);
    msg.data = (uint8_t *)data;
    msg.length = len;
    msg.done = wait ? xSemaphoreCreateBinary() : NULL;

    if (xQueueSend(s_io_queue, &msg, pdMS_TO_TICKS(100)) != pdTRUE) {
        if (msg.done) vSemaphoreDelete(msg.done);
        return ESP_FAIL;
    }

    if (wait) {
        xSemaphoreTake(msg.done, portMAX_DELAY);
        esp_err_t res = msg.result;
        vSemaphoreDelete(msg.done);
        return res;
    }

    return ESP_OK;
}

esp_err_t sd_queue_append(const char *path, const uint8_t *data, size_t len, bool wait)
{
    if (!s_card_mounted || !s_io_queue) return ESP_ERR_SD_NOT_MOUNTED;

    sd_io_msg_t msg = {0};
    msg.type = SD_IO_APPEND;
    strncpy(msg.path, path, sizeof(msg.path) - 1);
    msg.data = (uint8_t *)data;
    msg.length = len;
    msg.done = wait ? xSemaphoreCreateBinary() : NULL;

    xQueueSend(s_io_queue, &msg, portMAX_DELAY);

    if (wait) {
        xSemaphoreTake(msg.done, portMAX_DELAY);
        esp_err_t res = msg.result;
        vSemaphoreDelete(msg.done);
        return res;
    }

    return ESP_OK;
}

static void sd_io_task(void *arg)
{
    sd_io_msg_t msg;
    ESP_LOGI(TAG, "async I/O task running");

    while (1) {
        if (!xQueueReceive(s_io_queue, &msg, portMAX_DELAY))
            continue;

        if (msg.type == SD_IO_EXIT)
            break;

        FILE *f = NULL;
        msg.result = ESP_FAIL;

        if (msg.type == SD_IO_WRITE)
            f = fopen(msg.path, "w");
        else if (msg.type == SD_IO_APPEND)
            f = fopen(msg.path, "a");

        if (f) {
            size_t written = fwrite(msg.data, 1, msg.length, f);
            fclose(f);
            msg.result = (written > 0) ? ESP_OK : ESP_FAIL;
        }

        if (msg.done)
            xSemaphoreGive(msg.done);
    }

    ESP_LOGI(TAG, "async sd task exit");
    vTaskDelete(NULL);
}

bool sd_is_mounted(void)
{
    return s_card_mounted;
}

esp_err_t sd_get_card_info(struct sdmmc_card_t **card_out)
{
    if (!card_out) return ESP_ERR_INVALID_ARG;
    if (!s_card_mounted) return ESP_ERR_SD_NOT_MOUNTED;

    *card_out = (struct sdmmc_card_t *)s_card;
    return ESP_OK;
}

static esp_err_t sd_check_mounted(void)
{
    return s_card_mounted ? ESP_OK : ESP_ERR_SD_NOT_MOUNTED;
}

esp_err_t sd_file_exists(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t sd_mkdir(const char *path)
{
    return (mkdir(path, 0775) == 0) ? ESP_OK : ESP_FAIL;
}
