#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "esp_timer.h"
#include "board_config.h"

typedef enum {
    SLEEP_NONE,
    SLEEP_LIGHT,
    SLEEP_DEEP,
} sleep_state_t;

#define SLEEP_LIGHT_TIMEOUT_MS  30000
#define SLEEP_DEEP_TIMEOUT_MS   120000

#define BATTERY_ADC_CHANNEL     BOARD_BATTERY_ADC_CHANNEL
#define BATTERY_ADC_UNIT        BOARD_BATTERY_ADC_UNIT

void sleep_manager_init(esp_timer_handle_t lvgl_tick_timer);
void sleep_manager_activity(void);
void sleep_manager_set_inhibited(bool inhibited);
sleep_state_t sleep_manager_get_state(void);
uint32_t sleep_manager_get_battery_voltage(void);
uint8_t sleep_manager_get_battery_percent(void);

#ifdef __cplusplus
}
#endif
