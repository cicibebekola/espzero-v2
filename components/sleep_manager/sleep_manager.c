#include "sleep_manager.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include <stdbool.h>

#define TAG "SLEEP_MGR"
#define BATTERY_VMIN_MV     3000
#define BATTERY_VMAX_MV     4200
#define BATTERY_ADC_ATTEN   ADC_ATTEN_DB_12

static sleep_state_t current_state = SLEEP_NONE;
static TimerHandle_t inactivity_timer = NULL;
static esp_timer_handle_t lvgl_tick_timer = NULL;
static adc_oneshot_unit_handle_t adc_handle = NULL;
static esp_timer_handle_t wake_timer = NULL;
static bool sleep_inhibited = false;

static void inactivity_callback(TimerHandle_t timer);
static void xpt2046_irq_isr(void *arg);
static void battery_adc_init_timer_cb(void *arg);
static void wake_deferred_cb(void *arg);

void sleep_manager_init(esp_timer_handle_t tick_timer)
{
    lvgl_tick_timer = tick_timer;
    current_state = SLEEP_NONE;

    inactivity_timer = xTimerCreate(
        "inactivity",
        pdMS_TO_TICKS(SLEEP_LIGHT_TIMEOUT_MS),
        pdFALSE,
        NULL,
        inactivity_callback
    );

    xTimerStart(inactivity_timer, 0);

    gpio_config_t irq_config = {
        .pin_bit_mask = BIT64(BOARD_TOUCH_IRQ_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&irq_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure GPIO %d for XPT2046 IRQ: %s",
                 BOARD_TOUCH_IRQ_GPIO, esp_err_to_name(ret));
        return;
    }

    ret = gpio_install_isr_service(0);
    if (ret == ESP_ERR_INVALID_STATE) {
        ESP_LOGD(TAG, "GPIO ISR service already installed");
    } else if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to install GPIO ISR service: %s", esp_err_to_name(ret));
        return;
    }

    ret = gpio_isr_handler_add((gpio_num_t)BOARD_TOUCH_IRQ_GPIO, xpt2046_irq_isr, NULL);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to add GPIO ISR handler: %s", esp_err_to_name(ret));
        return;
    }

    esp_timer_create_args_t wake_args = {
        .callback = wake_deferred_cb,
        .name = "sleep_wake",
        .dispatch_method = ESP_TIMER_TASK,
    };
    esp_timer_create(&wake_args, &wake_timer);

    ESP_LOGI(TAG, "Sleep manager initialized (XPT2046 IRQ wake enabled)");

    esp_timer_create_args_t timer_args = {
        .callback = battery_adc_init_timer_cb,
        .name = "battery_adc_init_timer"
    };
    esp_timer_handle_t adc_init_timer;
    ret = esp_timer_create(&timer_args, &adc_init_timer);
    if (ret == ESP_OK) {
        esp_timer_start_once(adc_init_timer, 1000000);
    }
}

void sleep_manager_activity(void)
{
    if (current_state == SLEEP_LIGHT) {
        ESP_LOGI(TAG, "Waking from light sleep");

        gpio_set_intr_type((gpio_num_t)BOARD_TOUCH_IRQ_GPIO, GPIO_INTR_DISABLE);
        gpio_intr_disable((gpio_num_t)BOARD_TOUCH_IRQ_GPIO);

        esp_timer_start_periodic(lvgl_tick_timer, 10 * 1000);

        current_state = SLEEP_NONE;
    }

    xTimerReset(inactivity_timer, 0);
}

void sleep_manager_set_inhibited(bool inhibited)
{
    sleep_inhibited = inhibited;
    if (inactivity_timer == NULL) return;

    if (inhibited) {
        if (current_state == SLEEP_LIGHT) {
            sleep_manager_activity();
        }
        xTimerStop(inactivity_timer, 0);
        ESP_LOGI(TAG, "Sleep inhibited");
    } else {
        xTimerReset(inactivity_timer, 0);
        ESP_LOGI(TAG, "Sleep enabled");
    }
}

sleep_state_t sleep_manager_get_state(void)
{
    return current_state;
}

static void inactivity_callback(TimerHandle_t timer)
{
    (void)timer;

    if (sleep_inhibited) {
        return;
    }

    if (current_state == SLEEP_NONE) {
        ESP_LOGI(TAG, "Entering light sleep");

        esp_timer_stop(lvgl_tick_timer);

        current_state = SLEEP_LIGHT;

        gpio_set_intr_type((gpio_num_t)BOARD_TOUCH_IRQ_GPIO, GPIO_INTR_NEGEDGE);
        gpio_intr_enable((gpio_num_t)BOARD_TOUCH_IRQ_GPIO);

        xTimerDelete(inactivity_timer, 0);
        inactivity_timer = xTimerCreate(
            "inactivity",
            pdMS_TO_TICKS(SLEEP_DEEP_TIMEOUT_MS),
            pdFALSE,
            NULL,
            inactivity_callback
        );
        xTimerStart(inactivity_timer, 0);
    }
    else if (current_state == SLEEP_LIGHT) {
        ESP_LOGI(TAG, "Entering deep sleep (wake on XPT2046 IRQ)");

        esp_sleep_enable_ext0_wakeup((gpio_num_t)BOARD_TOUCH_IRQ_GPIO, 0);
        esp_deep_sleep_start();
    }
}

static void wake_deferred_cb(void *arg)
{
    (void)arg;

    int low_count = 0;
    for (int i = 0; i < 5; i++) {
        if (gpio_get_level((gpio_num_t)BOARD_TOUCH_IRQ_GPIO) == 0) {
            low_count++;
        }
        esp_rom_delay_us(1000);
    }

    if (low_count < 3) {
        ESP_LOGD(TAG, "Spurious wake ignored (low_count=%d)", low_count);
        gpio_intr_enable((gpio_num_t)BOARD_TOUCH_IRQ_GPIO);
        return;
    }

    sleep_manager_activity();
}

static void xpt2046_irq_isr(void *arg)
{
    (void)arg;
    if (current_state == SLEEP_LIGHT && wake_timer != NULL) {
        gpio_intr_disable((gpio_num_t)BOARD_TOUCH_IRQ_GPIO);
        esp_timer_start_once(wake_timer, 0);
    }
}

static void battery_adc_init_timer_cb(void *arg)
{
    (void)arg;

    adc_oneshot_unit_init_cfg_t init_config = {
        .unit_id = BATTERY_ADC_UNIT,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    esp_err_t ret = adc_oneshot_new_unit(&init_config, &adc_handle);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to initialize ADC: %s", esp_err_to_name(ret));
        return;
    }

    adc_oneshot_chan_cfg_t chan_config = {
        .atten = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_12,
    };

    ret = adc_oneshot_config_channel(adc_handle, BATTERY_ADC_CHANNEL, &chan_config);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to configure ADC channel: %s", esp_err_to_name(ret));
    } else {
        ESP_LOGI(TAG, "Battery ADC initialized (GPIO34)");
    }
}

uint32_t sleep_manager_get_battery_voltage(void)
{
    if (adc_handle == NULL) {
        return 0;
    }

    int adc_reading = 0;
    esp_err_t ret = adc_oneshot_read(adc_handle, BATTERY_ADC_CHANNEL, &adc_reading);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to read battery ADC: %s", esp_err_to_name(ret));
        return 0;
    }

    // R2=100K, R3=100K voltage divider: V_adc = V_bat / 2
    uint32_t voltage_mv = ((uint32_t)adc_reading * 3600 * 2) / 4095;
    return voltage_mv;
}

uint8_t sleep_manager_get_battery_percent(void)
{
    uint32_t voltage_mv = sleep_manager_get_battery_voltage();

    if (voltage_mv >= BATTERY_VMAX_MV) {
        return 100;
    } else if (voltage_mv <= BATTERY_VMIN_MV) {
        return 0;
    }

    uint32_t range = BATTERY_VMAX_MV - BATTERY_VMIN_MV;
    uint32_t offset = voltage_mv - BATTERY_VMIN_MV;
    uint8_t percent = (offset * 100) / range;

    return percent;
}
