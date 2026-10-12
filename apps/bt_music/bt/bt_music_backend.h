#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/* Maximum device name length including null terminator */
#define BT_DEVICE_NAME_MAX  64
#define BT_ADDR_STR_LEN     18   /* "AA:BB:CC:DD:EE:FF\0" */
#define BT_MAX_SCAN_RESULTS 12

typedef struct {
    char     name[BT_DEVICE_NAME_MAX];
    uint8_t  addr[6];
    int8_t   rssi;
} bt_device_info_t;

typedef enum {
    BT_STATE_IDLE,
    BT_STATE_SCANNING,
    BT_STATE_CONNECTING,
    BT_STATE_CONNECTED,
    BT_STATE_DISCONNECTED,
} bt_conn_state_t;

/* Callbacks — called from BT task, use lv_async_call for any LVGL work */
typedef void (*bt_scan_result_cb_t)(const bt_device_info_t *dev, void *arg);
typedef void (*bt_conn_state_cb_t)(bt_conn_state_t state, const char *device_name, void *arg);

/* --- Init / deinit --- */
esp_err_t bt_music_init(void);
void      bt_music_deinit(void);

/* --- Scanning --- */
/* Active scan: populates a visible device list, fires cb for every result */
esp_err_t bt_music_start_scan(bt_scan_result_cb_t cb, void *arg);
void      bt_music_stop_scan(void);
/* Passive/background scan: keeps discoverable + connectable so swift-connect
 * works without user action. Does not fire scan result callbacks. */
void      bt_music_start_passive(void);
void      bt_music_stop_passive(void);

/* --- Connection --- */
esp_err_t bt_music_connect(const uint8_t addr[6], bt_conn_state_cb_t cb, void *arg);
void      bt_music_disconnect(void);
bool      bt_music_is_connected(void);
bt_conn_state_t bt_music_get_state(void);
void      bt_music_get_connected_name(char *buf, size_t len);

/* --- Stream control --- */
/* Call start_stream when audio data is ready to flow; stop_stream to pause */
void bt_music_start_stream(void);
void bt_music_stop_stream(void);

/* Tone generator: feeds a 440Hz sine into the stream for testing (no SD card needed) */
void bt_music_set_tone(bool enable);

/* --- A2DP source data feed (called internally by audio pipeline) --- */
/* mp3_player calls this to push PCM frames into the A2DP source callback */
bool bt_music_feed_pcm(const int16_t *pcm, size_t frames);
void bt_music_clear_pcm(void);
size_t bt_music_pending_pcm(void);
int bt_music_sample_rate(void);

/* Volume: 0-127 (A2DP AVRCP absolute volume) */
void bt_music_set_volume(uint8_t vol);
uint8_t bt_music_get_volume(void);
