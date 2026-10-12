#pragma once

#include "esp_adc/adc_oneshot.h"
#include "driver/spi_master.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── SD Card (SPI3) ── */
#define BOARD_SD_PIN_CS          5
#define BOARD_SD_PIN_MOSI        23
#define BOARD_SD_PIN_MISO        19
#define BOARD_SD_PIN_CLK         18
#define BOARD_SD_SPI_HOST        SPI3_HOST
#define BOARD_SD_SPI_FREQ_KHZ    20000

/* ── Touch IRQ ── */
#define BOARD_TOUCH_IRQ_GPIO     36
#define BOARD_TOUCH_CS_GPIO      33

/* ── Battery ADC (voltage divider: R2=100K, R3=100K → V_adc = V_bat / 2) ── */
#define BOARD_BATTERY_ADC_CHANNEL ADC_CHANNEL_6
#define BOARD_BATTERY_ADC_UNIT    ADC_UNIT_1
#define BOARD_BATTERY_ADC_GPIO    34

/* ── RGB LED (common anode, low = lit) ── */
#define BOARD_LED_RED_GPIO       17
#define BOARD_LED_GREEN_GPIO     22
#define BOARD_LED_BLUE_GPIO      16

/* ── Display ── (esp_lcd handles pin config, these are for ref) ── */
#define BOARD_LCD_BL_GPIO        27

#ifdef __cplusplus
}
#endif
