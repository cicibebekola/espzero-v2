#pragma once
/**
 * @file sd_card_manager.h
 * @brief SD Card Manager for ESP32 using SPI (SPI3_HOST)
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/unistd.h>
#include "esp_err.h"
#include "board_config.h"
#include "device_paths.h"

struct sdmmc_card_t;

#define SD_MOUNT_POINT       DEVICE_SD_MOUNT_POINT
#define SD_MAX_OPEN_FILES    5
#define SD_ALLOCATION_UNIT_SIZE (16 * 1024)

#define SD_PIN_CS   BOARD_SD_PIN_CS
#define SD_PIN_MOSI BOARD_SD_PIN_MOSI
#define SD_PIN_MISO BOARD_SD_PIN_MISO
#define SD_PIN_CLK  BOARD_SD_PIN_CLK
#define SD_SPI_HOST BOARD_SD_SPI_HOST
#define SD_MAX_TRANSFER_SIZE 2048
#define SD_SPI_FREQ_KHZ BOARD_SD_SPI_FREQ_KHZ

#define ESP_ERR_SD_BASE         0x6000
#define ESP_ERR_SD_NOT_MOUNTED  (ESP_ERR_SD_BASE + 1)
#define ESP_ERR_SD_MOUNT_FAILED (ESP_ERR_SD_BASE + 2)
#define ESP_ERR_SD_FILE_FAILED  (ESP_ERR_SD_BASE + 3)

esp_err_t sd_init(void);
esp_err_t sd_card_init(bool format_if_failed);
esp_err_t sd_card_deinit(void);
esp_err_t sd_write_file(const char *path, const char *data);
esp_err_t sd_read_file(const char *path, char *buffer, size_t buffer_size);

typedef struct {
    char name[64];
    bool is_dir;
} sd_file_t;

int sd_list_files(const char *path);
char **sd_get_file_array(void);
esp_err_t sd_list_files_struct(const char *path, sd_file_t *files, size_t max_files, size_t *out_count);
esp_err_t sd_append_file(const char *path, const char *data);
bool sd_is_mounted(void);
esp_err_t sd_get_card_info(struct sdmmc_card_t **card_info);
esp_err_t sd_file_exists(const char *path);
esp_err_t sd_mkdir(const char *path);

esp_err_t sd_queue_write(const char *path, const uint8_t *data, size_t len, bool wait);
esp_err_t sd_queue_append(const char *path, const uint8_t *data, size_t len, bool wait);

#define SD_PATH(filename) (SD_MOUNT_POINT "/" filename)
#define SD_FILE_EXISTS(path) (access(path, F_OK) == 0)
#define SD_DELETE_FILE(path) (unlink(path) == 0)

#ifdef __cplusplus
}
#endif
