#include "mp3_player.h"
#include "bt_music_backend.h"
#include "pcm_resampler.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_random.h"
#include "minimp3.h"

#define TAG "MP3_PLAYER"
#define READ_BUF_SIZE 4096
#define TASK_STACK (24 * 1024) /* measured decode stack: 17,344 bytes + callers */
static mp3_track_t *s_tracks;
static int s_track_count;
static volatile int s_current_idx = -1;
static volatile mp3_state_t s_state = MP3_STATE_IDLE;
static volatile bool s_shuffle, s_repeat;
static volatile uint32_t s_elapsed_sec, s_duration_sec;
static const char *s_error = "";
static mp3_event_cb_t s_event_cb;
static void *s_event_arg;
static volatile TaskHandle_t s_task;
static portMUX_TYPE s_command_mux = portMUX_INITIALIZER_UNLOCKED;
typedef enum {CMD_NONE, CMD_PLAY, CMD_PAUSE, CMD_RESUME, CMD_STOP, CMD_NEXT, CMD_PREV, CMD_QUIT} player_cmd_t;
static player_cmd_t s_cmd;
static int s_cmd_idx;
static void command(player_cmd_t cmd, int index)
{
    portENTER_CRITICAL(&s_command_mux);
    s_cmd_idx = index; s_cmd = cmd;
    portEXIT_CRITICAL(&s_command_mux);
}
static player_cmd_t peek_command(void)
{
    portENTER_CRITICAL(&s_command_mux);
    player_cmd_t cmd = s_cmd;
    portEXIT_CRITICAL(&s_command_mux);
    return cmd;
}
static player_cmd_t take_command(int *index)
{
    portENTER_CRITICAL(&s_command_mux);
    player_cmd_t cmd = s_cmd;
    *index = s_cmd_idx; s_cmd = CMD_NONE;
    portEXIT_CRITICAL(&s_command_mux);
    return cmd;
}
static bool take_control(player_cmd_t expected)
{
    portENTER_CRITICAL(&s_command_mux);
    bool matches = s_cmd == expected;
    if (matches) s_cmd = CMD_NONE;
    portEXIT_CRITICAL(&s_command_mux);
    return matches;
}
static void notify(mp3_state_t state, int index)
{
    s_state = state;
    if (s_event_cb) s_event_cb(state, index, s_event_arg);
}
static void fail(const char *message)
{
    s_error = message;
    ESP_LOGE(TAG, "%s", message);
    notify(MP3_STATE_ERROR, s_current_idx);
}
/* Minimal ID3v2 skip: skip the tag at start of file so we
 * give minimp3 clean MPEG frames. */
static void skip_id3v2(FILE *f)
{
    uint8_t hdr[10];
    if (fread(hdr, 1, 10, f) < 10) { rewind(f); return; }
    if (hdr[0]=='I' && hdr[1]=='D' && hdr[2]=='3') {
        uint32_t sz = ((hdr[6]&0x7f)<<21)|((hdr[7]&0x7f)<<14)|
                      ((hdr[8]&0x7f)<<7)|(hdr[9]&0x7f);
        sz += 10; /* include header itself */
        fseek(f, sz, SEEK_SET);
        ESP_LOGD(TAG, "Skipped ID3v2 tag: %u bytes", sz);
    } else {
        rewind(f);
    }
}

/* Read ID3v1 tag at end of file for title/artist (128 bytes, fixed) */
static void read_id3v1(const char *path, mp3_track_t *track)
{
    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, -128, SEEK_END) == 0) {
        uint8_t tag[128];
        if (fread(tag, 1, 128, f) == 128 && tag[0]=='T' && tag[1]=='A' && tag[2]=='G') {
            /* Title: bytes 3-32 */
            char tmp[31];
            memcpy(tmp, tag+3, 30); tmp[30]='\0';
            /* Strip trailing spaces */
            int l = strlen(tmp);
            while (l > 0 && tmp[l-1]==' ') tmp[--l]='\0';
            if (l > 0) strncpy(track->title, tmp, MP3_TAG_MAX-1);

            /* Artist: bytes 33-62 */
            memcpy(tmp, tag+33, 30); tmp[30]='\0';
            l = strlen(tmp);
            while (l > 0 && tmp[l-1]==' ') tmp[--l]='\0';
            if (l > 0) strncpy(track->artist, tmp, MP3_TAG_MAX-1);
        }
    }
    fclose(f);
}


static bool service_playback(void)
{
    bool paused = s_state == MP3_STATE_PAUSED;
    for (;;) {
        player_cmd_t cmd = peek_command();
        if (cmd == CMD_PLAY || cmd == CMD_NEXT || cmd == CMD_PREV || cmd == CMD_STOP || cmd == CMD_QUIT)
            return false; /* Leave transport commands for the outer task. */
        if (!bt_music_is_connected()) { fail("Bluetooth disconnected. Connect headphones and press Play."); return false; }
        if (cmd == CMD_PAUSE || cmd == CMD_RESUME) {
            if (!take_control(cmd)) continue;
            paused = cmd == CMD_PAUSE;
            if (paused) { bt_music_stop_stream(); notify(MP3_STATE_PAUSED, s_current_idx); }
            else { bt_music_start_stream(); notify(MP3_STATE_PLAYING, s_current_idx); }
        }
        if (!paused) return true;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static bool send_pcm(const int16_t *pcm, size_t frames, void *arg)
{
    (void)arg;
    TickType_t stalled = 0;
    while (service_playback()) {
        if (bt_music_feed_pcm(pcm, frames)) return true;
        stalled += pdMS_TO_TICKS(20);
        if (stalled > pdMS_TO_TICKS(5000)) {
            fail("Bluetooth audio stalled. Reconnect headphones and try again.");
            return false;
        }
    }
    return false;
}

/* Returns true only for clean EOF, never for Stop, another track, or an error. */
static bool play_file(const char *path)
{
    if (!bt_music_is_connected()) { fail("Connect Bluetooth headphones before playing."); return false; }
    FILE *file = fopen(path, "rb");
    if (!file) { fail("Cannot open MP3. Check the SD card and file."); return false; }
    skip_id3v2(file);
    long audio_start = ftell(file);
    struct stat st; size_t audio_bytes = stat(path, &st) == 0 && st.st_size > audio_start ? st.st_size - audio_start : 0;
    uint8_t *input = malloc(READ_BUF_SIZE);
    int16_t *pcm = malloc(MINIMP3_MAX_SAMPLES_PER_FRAME * sizeof(int16_t));
    mp3dec_t *decoder = malloc(sizeof(mp3dec_t));
    if (!input || !pcm || !decoder) {
        free(input); free(pcm); free(decoder); fclose(file);
        fail("Not enough memory to start MP3 playback."); return false;
    }
    mp3dec_init(decoder);
    pcm_resampler_t resampler = {0};
    size_t used = 0; uint64_t played_us = 0;
    bool eof = false, clean_end = false, decoded = false;
    bt_music_set_tone(false);
    bt_music_clear_pcm();
    bt_music_start_stream();
    notify(MP3_STATE_PLAYING, s_current_idx);
    ESP_LOGI(TAG, "Streaming %s (4 KB reads, bounded PCM chunks)", path);
    while (service_playback()) {
        if (!eof && used < READ_BUF_SIZE) {
            size_t read = fread(input + used, 1, READ_BUF_SIZE - used, file);
            used += read;
            if (ferror(file)) { fail("SD card read failed during playback."); break; }
            eof = feof(file);
        }
        if (!used) { clean_end = decoded; if (!decoded) fail("No playable MP3 audio found."); break; }
        mp3dec_frame_info_t info = {0};
        int frames = mp3dec_decode_frame(decoder, input, used, pcm, &info);
        if (info.frame_bytes <= 0) {
            if (eof) { clean_end = decoded; if (!decoded) fail("Invalid or unsupported MP3 file."); break; }
            /* Decoder found no complete frame in a full buffer. Keep enough
             * tail bytes for a frame crossing the next SD read boundary. */
            size_t keep = used < 2048 ? used : 2048;
            memmove(input, input + used - keep, keep); used = keep;
            vTaskDelay(1);
            continue;
        }
        if ((size_t)info.frame_bytes > used) { fail("Invalid MP3 frame."); break; }
        used -= info.frame_bytes; memmove(input, input + info.frame_bytes, used);
        if (frames > 0) {
            if (!decoded) {
                decoded = true;
                s_duration_sec = info.bitrate_kbps > 0 ? (uint64_t)audio_bytes * 8 / (info.bitrate_kbps * 1000) : 0;
                ESP_LOGI(TAG, "MP3: %d Hz, %d channel(s), %d kbps", info.hz, info.channels, info.bitrate_kbps);
            }
            /* minimp3 returns frames PER CHANNEL, not total stereo samples. */
            if (!pcm_resample(&resampler, pcm, frames, info.channels, info.hz, bt_music_sample_rate(), send_pcm, NULL)) {
                if (peek_command() == CMD_NONE && s_state != MP3_STATE_ERROR) fail("Unsupported MP3 sample rate.");
                break;
            }
            played_us += (uint64_t)frames * 1000000 / info.hz;
            s_elapsed_sec = played_us / 1000000;
        }
    }
    /* Let the final buffered audio drain instead of cutting off the song. */
    TickType_t drain_start = xTaskGetTickCount();
    while (clean_end && bt_music_pending_pcm() && service_playback()) {
        if (xTaskGetTickCount() - drain_start > pdMS_TO_TICKS(1000)) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (peek_command() != CMD_NONE || s_state == MP3_STATE_ERROR) clean_end = false;
    bt_music_stop_stream();
    bt_music_clear_pcm();
    free(input); free(pcm); free(decoder); fclose(file);
    return clean_end;
}

static int next_track_idx(void)
{
    if (!s_track_count) return -1;
    if (s_shuffle && s_track_count > 1) return (s_current_idx + 1 + esp_random() % (s_track_count-1)) % s_track_count;
    return (s_current_idx + 1) % s_track_count;
}
static int prev_track_idx(void)
{
    return s_track_count ? (s_current_idx <= 0 ? s_track_count - 1 : s_current_idx - 1) : -1;
}
static void player_task(void *arg)
{
    (void)arg;
    int automatic = -1;
    for (;;) {
        int index;
        player_cmd_t cmd = take_command(&index);
        if (cmd == CMD_QUIT) break;
        if (cmd == CMD_NEXT) { index = next_track_idx(); cmd = CMD_PLAY; }
        if (cmd == CMD_PREV) { index = prev_track_idx(); cmd = CMD_PLAY; }
        if (cmd == CMD_STOP) { automatic = -1; s_elapsed_sec = 0; notify(MP3_STATE_STOPPED, s_current_idx); }
        if (cmd == CMD_NONE && automatic >= 0) { index = automatic; cmd = CMD_PLAY; }
        automatic = -1;
        if (cmd == CMD_PLAY && index >= 0 && index < s_track_count) {
            s_current_idx = index; s_elapsed_sec = 0; s_duration_sec = 0; s_error = "";
            char path[sizeof(MP3_DIR) + MP3_NAME_MAX + 1];
            snprintf(path, sizeof(path), "%s/%s", MP3_DIR, s_tracks[index].name);
            bool ended = play_file(path);
            if (ended && peek_command() == CMD_NONE) {
                if (s_repeat) automatic = index;
                else if (s_shuffle || index + 1 < s_track_count) automatic = next_track_idx();
                if (automatic < 0) notify(MP3_STATE_STOPPED, index);
            }
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    bt_music_stop_stream(); bt_music_clear_pcm();
    notify(MP3_STATE_IDLE, -1);
    s_task = NULL;
    vTaskDelete(NULL);
}

esp_err_t mp3_player_init(mp3_event_cb_t cb, void *arg)
{
    if (s_task) return ESP_OK;
    s_event_cb = cb; s_event_arg = arg; s_cmd = CMD_NONE;
    TaskHandle_t task;
    if (xTaskCreatePinnedToCore(player_task, "mp3_player", TASK_STACK, NULL, 5, &task, 1) != pdPASS) return ESP_ERR_NO_MEM;
    s_task = task;
    return ESP_OK;
}
void mp3_player_deinit(void)
{
    if (s_task) { command(CMD_QUIT, 0); while (s_task) vTaskDelay(pdMS_TO_TICKS(10)); }
    free(s_tracks); s_tracks = NULL; s_track_count = 0; s_current_idx = -1;
}
static int track_compare(const void *a, const void *b)
{
    return strcasecmp(((const mp3_track_t *)a)->name, ((const mp3_track_t *)b)->name);
}
esp_err_t mp3_player_scan(void)
{
    if (s_state == MP3_STATE_PLAYING || s_state == MP3_STATE_PAUSED || peek_command() == CMD_PLAY)
        return ESP_ERR_INVALID_STATE;
    mp3_track_t *allocation = realloc(s_tracks, MP3_LIST_MAX * sizeof(*s_tracks));
    if (!allocation) return ESP_ERR_NO_MEM;
    s_tracks = allocation;
    s_track_count = 0; s_current_idx = -1;
    DIR *dir = opendir(MP3_DIR);
    if (!dir) { mkdir(MP3_DIR, 0775); dir = opendir(MP3_DIR); }
    if (!dir) {
        free(s_tracks); s_tracks = NULL;
        s_error = "No SD card / music folder. Upload MP3 files to /music.";
        return ESP_ERR_NOT_FOUND;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) && s_track_count < MP3_LIST_MAX) {
        size_t length = strlen(entry->d_name);
        if (length < 5 || length >= MP3_NAME_MAX || entry->d_name[0] == '.' || strcasecmp(entry->d_name+length-4, ".mp3")) continue;
        char path[sizeof(MP3_DIR) + MP3_NAME_MAX + 1]; struct stat st;
        snprintf(path, sizeof(path), "%s/%.*s", MP3_DIR, MP3_NAME_MAX - 1, entry->d_name);
        if (stat(path, &st) || !S_ISREG(st.st_mode)) continue;
        mp3_track_t *track = &s_tracks[s_track_count++];
        memset(track, 0, sizeof(*track)); strcpy(track->name, entry->d_name);
        size_t title_length = length - 4 < MP3_TAG_MAX - 1 ? length - 4 : MP3_TAG_MAX - 1;
        memcpy(track->title, entry->d_name, title_length);
        read_id3v1(path, track);
    }
    closedir(dir);
    qsort(s_tracks, s_track_count, sizeof(*s_tracks), track_compare);
    /* A one-song library should not permanently reserve all 32 entries. */
    if (s_track_count) {
        allocation = realloc(s_tracks, s_track_count * sizeof(*s_tracks));
        if (allocation) s_tracks = allocation;
    } else { free(s_tracks); s_tracks = NULL; }
    s_error = s_track_count ? "" : "No MP3 files. Upload songs to /music and reopen Music.";
    ESP_LOGI(TAG, "%d MP3 tracks in %s (limit %d)", s_track_count, MP3_DIR, MP3_LIST_MAX);
    return ESP_OK;
}
int mp3_player_track_count(void) { return s_track_count; }
const mp3_track_t *mp3_player_get_track(int idx) { return idx >= 0 && idx < s_track_count ? &s_tracks[idx] : NULL; }
int mp3_player_get_current_idx(void) { return s_current_idx; }
esp_err_t mp3_player_play(int idx)
{
    if (!s_task) return ESP_ERR_INVALID_STATE;
    if (idx < 0 || idx >= s_track_count) return ESP_ERR_INVALID_ARG;
    command(CMD_PLAY, idx); return ESP_OK;
}
void mp3_player_pause(void) { command(CMD_PAUSE, 0); }
void mp3_player_resume(void) { command(CMD_RESUME, 0); }
void mp3_player_stop(void) { command(CMD_STOP, 0); }
void mp3_player_next(void) { command(CMD_NEXT, 0); }
void mp3_player_prev(void) { command(CMD_PREV, 0); }
mp3_state_t mp3_player_get_state(void) { return s_state; }
uint32_t mp3_player_get_elapsed_sec(void) { return s_elapsed_sec; }
uint32_t mp3_player_get_duration_sec(void) { return s_duration_sec; }
const char *mp3_player_get_error(void) { return s_error; }
void mp3_player_set_shuffle(bool enabled) { s_shuffle = enabled; }
void mp3_player_set_repeat(bool enabled) { s_repeat = enabled; }
bool mp3_player_get_shuffle(void) { return s_shuffle; }
bool mp3_player_get_repeat(void) { return s_repeat; }
