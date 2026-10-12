/*
 * tp_spi.h — SPI communication for XPT2046 touch controller
 */
#ifndef TP_SPI_H
#define TP_SPI_H

#include <stdint.h>
#include <driver/spi_master.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SPI config */
#define SPI_TOUCH_CLOCK_SPEED_HZ  (2 * 1000 * 1000)  /* 2 MHz */
#define SPI_TOUCH_SPI_MODE        0

void tp_spi_add_device(spi_host_device_t host);
void tp_spi_xchg(uint8_t *data_send, uint8_t *data_recv, uint8_t byte_count);
void tp_spi_write_reg(uint8_t *data, uint8_t byte_count);
void tp_spi_read_reg(uint8_t reg, uint8_t *data, uint8_t byte_count);

#ifdef __cplusplus
}
#endif

#endif /* TP_SPI_H */
