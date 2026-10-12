# EspZero v2 — Project Context

Clean rewrite targeting **ESP-IDF 6.0.2 + LVGL 9** with `esp_lcd` + `esp_lvgl_port` (Espressif's official LVGL integration layer).

## Hardware: 3.2" ESP32-32E ST7789 (E32R32P/E32N32P)

### SPI Bus Assignment

| Bus | Devices | Pins |
|-----|---------|------|
| SPI2_HOST | ST7789 display (CS=15) + XPT2046 touch (CS=33) | MISO=12, MOSI=13, SCLK=14 |
| SPI3_HOST | SD card (CS=5) | MISO=19, MOSI=23, SCLK=18 |

GPIO12 is **shared MISO** for both display and touch on SPI2 (both share the same physical bus).

### GPIO Map

| Function | GPIO | Notes |
|----------|------|-------|
| LCD_MOSI | 13 | SPI2 shared |
| LCD_SCLK | 14 | SPI2 shared |
| LCD_MISO | 12 | SPI2 shared (touch DOUT too) |
| LCD_CS | 15 | |
| LCD_DC | 2 | |
| LCD_BL | 27 | High = ON, N-MOSFET |
| TOUCH_CS | 33 | |
| TOUCH_IRQ | 36 | INPUT_ONLY, used by sleep_manager |
| SD_CS | 5 | SPI3 |
| SD_MOSI | 23 | SPI3 |
| SD_MISO | 19 | SPI3 |
| SD_SCLK | 18 | SPI3 |
| LED_RED | 17 | Common anode, low = lit |
| LED_GREEN | 22 | |
| LED_BLUE | 16 | |
| BAT_ADC | 34 | ADC1_CH6, voltage divider R=100K/100K |

### Display
- Panel: ST7789, 240×320, 16-bit RGB565
- Default orientation: native portrait (app can change via `lvgl_runtime_set_rotation()`)
- Backlight: GPIO27, active high
- No hardware reset pin (software reset only)
- Color inversion needed (typical for ST7789 with BGR panels)

### Touch
- XPT2046 resistive touch, SPI protocol
- IRQ: GPIO36 (INPUT_ONLY pin, no pullup — external 10K pull-up on PCB)
- Shares the SPI2 bus with the display

### Battery
- ADC1_CHANNEL_6 on GPIO34
- Voltage divider: R2=100K, R3=100K → V_adc = V_bat/2
- Li-Ion: 3.0V (empty) – 4.2V (full)

## Build Model

Root `CMakeLists.txt` selects one active app via `ACTIVE_APP`:

```
idf.py -DACTIVE_APP=launcher build
```

Default: `launcher`

`main/` is a dummy component — real `app_main()` comes from `apps/<ACTIVE_APP>/`.

## Component Dependencies (ESP-IDF 6.x)

- `lvgl_runtime` → esp_lcd, esp_lvgl_port (from component registry), esp_driver_spi, esp_driver_gpio, device_config
- `sd_card` → driver, fatfs, sdmmc, esp_driver_spi, device_config
- `sleep_manager` → driver, esp_timer, esp_adc (oneshot API), esp_rom, device_config
- `ota_loader` → app_update, esp_partition
- `device_config` → esp_adc, driver (for SPI3_HOST type)

## External Components (via idf_component.yml)

- `espressif/esp_lvgl_port ^2.4.0` — official LVGL integration glue
- `lvgl/lvgl ~9.2` — LVGL 9.x
- `espressif/esp_lcd_touch_xpt2046 ^1.0.0` — XPT2046 touch driver for esp_lcd

## Partition Layout

Same as EspZero-main:
- ota_0 / ota_1: 1.5 MB each
- FAT storage: 768 KB
- NVS: 24 KB + OTA data 8 KB

## Apps (to be ported)

- `launcher` — minimal test (Phase 1, done)
- `app_manager` — WiFi/http/OTA manager + SD file browser
- `bt_music` — Bluetooth A2DP source + MP3 player

## LVGL 9 Migration Notes

- `lv_timer_handler()` replaces `lv_task_handler()`
- `lv_screen_active()` replaces `lv_scr_act()`
- Widget creation APIs unchanged (lv_label_create, lv_btn_create, etc.)
- `lv_style_set_*` API unchanged
- No more `lv_disp_drv_t` / `lv_indev_drv_t` — esp_lvgl_port handles it
- No more `lv_disp_flush_ready()` — esp_lcd flush callback handles it
- `lv_async_call()` still exists (renamed `lv_async_timer`/`lv_async_call`)
