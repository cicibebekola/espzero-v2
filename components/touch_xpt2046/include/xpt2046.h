/*
 * xpt2046.h — XPT2046 resistive touch driver for LVGL 9
 * Calibration values from working 3.2inch_ESP32_LVGL project
 */
#ifndef XPT2046_H
#define XPT2046_H

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IRQ pin */
#define XPT2046_IRQ  36

/* Averaging window */
#define XPT2046_AVG  4

/* Calibration values (from working project sdkconfig) */
#define XPT2046_X_MIN  179
#define XPT2046_Y_MIN  176
#define XPT2046_X_MAX  1969
#define XPT2046_Y_MAX  1934

/* Axis correction flags */
#define XPT2046_XY_SWAP   1
#define XPT2046_X_INV     1
#define XPT2046_Y_INV     1

/* Touch detection */
#define XPT2046_TOUCH_THRESHOLD  400
#define XPT2046_TOUCH_IRQ        0   /* IRQ-only detection */
#define XPT2046_TOUCH_IRQ_PRESS  1  /* IRQ + pressure */
#define XPT2046_TOUCH_PRESS      0   /* Pressure-only detection (working config) */

/*
 * Initialize the XPT2046 and register as LVGL indev
 */
void xpt2046_init(void);

/*
 * LVGL 9 read callback (registered by xpt2046_init)
 */
void xpt2046_read_cb(lv_indev_t *indev, lv_indev_data_t *data);

#ifdef __cplusplus
}
#endif

#endif /* XPT2046_H */
