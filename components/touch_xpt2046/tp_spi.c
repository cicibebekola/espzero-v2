/*
 * tp_spi.c — SPI communication for XPT2046 touch controller
 * Ported from working 3.2inch_ESP32_LVGL project
 */
#include "tp_spi.h"
#include "esp_system.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include <string.h>
#include "board_config.h"

static spi_device_handle_t s_touch_spi = NULL;

void tp_spi_add_device(spi_host_device_t host)
{
    spi_device_interface_config_t devcfg = {
        .clock_speed_hz = SPI_TOUCH_CLOCK_SPEED_HZ,
        .mode = SPI_TOUCH_SPI_MODE,
        .spics_io_num = BOARD_TOUCH_CS_GPIO,
        .queue_size = 1,
        .pre_cb = NULL,
        .post_cb = NULL,
        .command_bits = 8,
        .address_bits = 0,
        .dummy_bits = 0,
        .flags = SPI_DEVICE_HALFDUPLEX | SPI_DEVICE_NO_DUMMY,
    };
    esp_err_t ret = spi_bus_add_device(host, &devcfg, &s_touch_spi);
    assert(ret == ESP_OK);
}

void tp_spi_xchg(uint8_t *data_send, uint8_t *data_recv, uint8_t byte_count)
{
    spi_transaction_t t = {
        .length = byte_count * 8,
        .tx_buffer = data_send,
        .rx_buffer = data_recv,
    };
    esp_err_t ret = spi_device_transmit(s_touch_spi, &t);
    assert(ret == ESP_OK);
}

void tp_spi_write_reg(uint8_t *data, uint8_t byte_count)
{
    spi_transaction_t t = {
        .length = byte_count * 8,
        .tx_buffer = data,
        .flags = 0,
    };
    esp_err_t ret = spi_device_transmit(s_touch_spi, &t);
    assert(ret == ESP_OK);
}

void tp_spi_read_reg(uint8_t reg, uint8_t *data, uint8_t byte_count)
{
    spi_transaction_t t = {
        .length = (byte_count + sizeof(reg)) * 8,
        .rxlength = byte_count * 8,
        .cmd = reg,
        .rx_buffer = data,
        .flags = 0,
    };
    esp_err_t ret = spi_device_transmit(s_touch_spi, &t);
    assert(ret == ESP_OK);
}
