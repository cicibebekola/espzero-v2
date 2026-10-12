#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "device_paths.h"

#define MP3_DIR          DEVICE_MUSIC_DIR
#define MP3_NAME_MAX     128   /* max filename length (FATFS) */
#define MP3_TAG_MAX      64    /* max ID3 title/artist length */
#define MP3_LIST_MAX     32    /* max tracks in playlist */

typedef enum {
    MP3_STATE_IDLE,
    MP3_STATE_PLAYING,
    MP3_STATE_PAUSED,
    MP3_STATE_STOPPED,
    MP3_STATE_ERROR,
} mp3_state_t;

typedef struct {
    char name[MP3_NAME_MAX];  /* filename without path */
    char title[MP3_TAG_MAX];  /* ID3 title or filename stem */
    char artist[MP3_TAG_MAX]; /* ID3 artist or empty */
} mp3_track_t;

/* Callback fired (from mp3 task) when track changes or state changes.
 * Use lv_async_call inside the callback for any LVGL work. */
typedef void (*mp3_event_cb_t)(mp3_state_t state, int track_idx, void *arg);

/* --- Init --- */
esp_err_t mp3_player_init(mp3_event_cb_t cb, void *arg);
void      mp3_player_deinit(void);

/* --- Playlist (scans /sdcard/music, mp3 files) --- */
esp_err_t mp3_player_scan(void);
int       mp3_player_track_count(void);
const mp3_track_t *mp3_player_get_track(int idx);
int       mp3_player_get_current_idx(void);

/* --- Playback control --- */
esp_err_t mp3_player_play(int idx);   /* play specific track */
void      mp3_player_pause(void);
void      mp3_player_resume(void);
void      mp3_player_stop(void);
void      mp3_player_next(void);
void      mp3_player_prev(void);

/* --- State --- */
mp3_state_t mp3_player_get_state(void);
uint32_t    mp3_player_get_elapsed_sec(void);
uint32_t    mp3_player_get_duration_sec(void);
const char *mp3_player_get_error(void);

/* --- Shuffle / repeat --- */
void mp3_player_set_shuffle(bool en);
void mp3_player_set_repeat(bool en);
bool mp3_player_get_shuffle(void);
bool mp3_player_get_repeat(void);
