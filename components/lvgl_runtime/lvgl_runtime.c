#include "lvgl_runtime.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_check.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "board_config.h"

static const char *TAG = "lvgl_runtime";

/*
 * SPI host assignment (confirmed against hardware):
 *   SPI2_HOST → Display (CS=15) + XPT2046 Touch (CS=33), shared bus
 *   SPI3_HOST → SD card (CS=5), separate bus (see sd_card_manager)
 *
 * Shared SPI2 bus pins:
 *   MISO  GPIO12  (shared by display read-back and touch DOUT)
 *   MOSI  GPIO13  (shared)
 *   SCLK  GPIO14  (shared)
 *
 * Display-only:
 *   CS    GPIO15
 *   DC    GPIO2
 *   BL    GPIO27  (high = on, N-MOSFET gate)
 *   RST   none    (software reset only)
 *
 * Touch-only:
 *   CS    GPIO33
 *   IRQ   GPIO36  (managed by sleep_manager)
 */
#define LCD_SPI_HOST    SPI2_HOST

/* ── Shared SPI bus pins ── */
#define SPI_PIN_MISO    12
#define SPI_PIN_MOSI    13
#define SPI_PIN_SCLK    14

/* ── Display pins ── */
#define LCD_PIN_CS      15
#define LCD_PIN_DC      2
#define LCD_PIN_RST     -1
#define LCD_PIN_BL      27

/* ── Display geometry (native panel size) ── */
#define LCD_H_RES       240
#define LCD_V_RES       320
#ifdef ESPZERO_MUSIC_PROFILE
#define LCD_DRAW_ROWS   20
#define LCD_DOUBLE_BUFFER false
#else
#define LCD_DRAW_ROWS   40
#define LCD_DOUBLE_BUFFER true
#endif

/* ── SPI clock speeds ── */
#define LCD_SPI_FREQ_HZ   (40 * 1000 * 1000)   /* 40 MHz */

static uint32_t s_tick_inc_ms;

/* Saved for runtime rotation changes */
static esp_lcd_panel_handle_t s_lcd_panel = NULL;

static void lvgl_tick_cb(void *arg)
{
    (void)arg;
    lv_tick_inc(s_tick_inc_ms);
}

esp_err_t lvgl_runtime_init(uint32_t tick_inc_ms,
                            uint32_t tick_period_ms,
                            esp_timer_handle_t *out_tick_timer)
{
    s_tick_inc_ms = tick_inc_ms;

    /* ─── 1. SPI bus (shared by display + touch) ─── */
    ESP_LOGI(TAG, "SPI bus: MISO=%d MOSI=%d SCLK=%d", SPI_PIN_MISO, SPI_PIN_MOSI, SPI_PIN_SCLK);
    spi_bus_config_t bus_cfg = {
        .sclk_io_num     = SPI_PIN_SCLK,
        .mosi_io_num     = SPI_PIN_MOSI,
        .miso_io_num     = SPI_PIN_MISO,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = LCD_H_RES * LCD_DRAW_ROWS * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(LCD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO),
                        TAG, "SPI bus init failed");

    /* ─── 2. LCD panel IO (SPI) ─── */
    ESP_LOGI(TAG, "ST7789 panel IO (CS=%d DC=%d)", LCD_PIN_CS, LCD_PIN_DC);
    esp_lcd_panel_io_handle_t io_handle = NULL;
    esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = LCD_PIN_DC,
        .cs_gpio_num       = LCD_PIN_CS,
        .pclk_hz           = LCD_SPI_FREQ_HZ,
        .lcd_cmd_bits       = 8,
        .lcd_param_bits     = 8,
        .spi_mode           = 0,
        .trans_queue_depth  = 10,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_SPI_HOST,
                                                  &io_cfg, &io_handle),
                        TAG, "LCD panel IO create failed");

    /* ─── 3. ST7789 panel driver ─── */
    ESP_LOGI(TAG, "ST7789 driver (RST=%d)", LCD_PIN_RST);
    esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num  = LCD_PIN_RST,
        .rgb_ele_order   = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel  = 16,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7789(io_handle, &panel_cfg, &s_lcd_panel),
                        TAG, "ST7789 create failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_lcd_panel), TAG, "Panel reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_lcd_panel), TAG, "Panel init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_invert_color(s_lcd_panel, true), TAG, "Invert failed");

    /*
     * Default orientation: native portrait 240x320, no mirror/swap.
     * Apps can call esp_lcd_panel_swap_xy() / esp_lcd_panel_mirror() at
     * runtime to switch orientation without changing sdkconfig.
     */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_lcd_panel, false), TAG, "Swap XY failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_lcd_panel, false, false), TAG, "Mirror failed");

    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_lcd_panel, true), TAG, "Display on failed");

    /* ─── 4. Backlight PWM (LEDC) ─── */
    ESP_LOGI(TAG, "Backlight PWM on GPIO %d", LCD_PIN_BL);
    ledc_timer_config_t bl_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,  /* 0-1023 */
        .timer_num       = LEDC_TIMER_1,
        .freq_hz         = 5000,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ESP_RETURN_ON_ERROR(ledc_timer_config(&bl_timer), TAG, "LEDC timer failed");

    ledc_channel_config_t bl_channel = {
        .gpio_num   = LCD_PIN_BL,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_1,
        .timer_sel  = LEDC_TIMER_1,
        .duty       = 819,  /* default 80% */
        .hpoint     = 0,
    };
    ESP_RETURN_ON_ERROR(ledc_channel_config(&bl_channel), TAG, "LEDC channel failed");
    ESP_LOGI(TAG, "Backlight PWM: 5kHz, 10-bit, default 80%%");

    /* ─── 5. LVGL port init ─── */
    ESP_LOGI(TAG, "LVGL port init");
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority     = 4,
        .task_stack         = 8192,
        .task_affinity      = 0,  /* pin to Core0 — same core as app_main() for thread safety */
        .task_max_sleep_ms  = 50,
        .task_stack_caps    = MALLOC_CAP_DEFAULT,
        .timer_period_ms    = tick_period_ms,
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port init failed");

    /* ─── 6. Add display to LVGL port ─── */
    /* Music uses one 20-row DMA buffer (9.6 KB); Launcher retains its
     * two 40-row buffers (38.4 KB). Never allocate a full framebuffer. */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io_handle,
        .panel_handle  = s_lcd_panel,
        .buffer_size   = LCD_H_RES * LCD_DRAW_ROWS,
        .double_buffer = LCD_DOUBLE_BUFFER,
        .hres          = LCD_H_RES,
        .vres          = LCD_V_RES,
        .monochrome    = false,
        .rotation      = {  /* native portrait, no transform */
            .swap_xy  = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma   = true,
            .swap_bytes = true,  /* ST7789 needs big-endian RGB565 over SPI */
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (!disp) return ESP_ERR_NO_MEM;
    ESP_LOGI(TAG, "Display buffers: %u bytes", (unsigned)(LCD_H_RES * LCD_DRAW_ROWS * sizeof(uint16_t) * (LCD_DOUBLE_BUFFER ? 2 : 1)));
    (void)disp;  /* touch is handled by custom xpt2046 driver */

    /* ─── 7. Tick timer (sleep_manager uses this handle) ─── */
    const esp_timer_create_args_t tick_args = {
        .callback = lvgl_tick_cb,
        .name     = "lvgl_tick",
    };
    esp_timer_handle_t tick_timer = NULL;
    ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), TAG, "Tick timer create failed");
    ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, tick_period_ms * 1000),
                        TAG, "Tick timer start failed");

    if (out_tick_timer) {
        *out_tick_timer = tick_timer;
    }

    ESP_LOGI(TAG, "LVGL runtime ready (240x320 native portrait, ST7789 on SPI2)");
    return ESP_OK;
}

/* ── Backlight brightness control ── */

esp_err_t lvgl_runtime_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    if (percent < 10) percent = 10;  /* minimum 10% to keep screen visible */
    uint32_t duty = (uint32_t)percent * 1023 / 100;
    ESP_RETURN_ON_ERROR(ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1, duty),
                        TAG, "LED duty failed");
    ESP_RETURN_ON_ERROR(ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_1),
                        TAG, "LED update failed");
    return ESP_OK;
}

/* ── App-controlled rotation helpers ── */

esp_err_t lvgl_runtime_set_rotation(bool swap_xy, bool mirror_x, bool mirror_y)
{
    if (!s_lcd_panel) return ESP_ERR_INVALID_STATE;

    ESP_RETURN_ON_ERROR(esp_lcd_panel_swap_xy(s_lcd_panel, swap_xy), TAG, "swap_xy failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_mirror(s_lcd_panel, mirror_x, mirror_y), TAG, "mirror failed");
    return ESP_OK;
}

void lvgl_runtime_run(uint32_t handler_period_ms)
{
    ESP_LOGI(TAG, "Entering LVGL handler loop");
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(handler_period_ms));
        lv_timer_handler();
    }
}
