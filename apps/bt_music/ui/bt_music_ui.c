#include "bt_music_ui.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "mp3_player.h"
#include "bt_music_backend.h"
#include "sleep_manager.h"
#include "bt_scan_ui.h"
#include "ota_loader.h"
#include "esp_system.h"

static const char *TAG = "BT_UI";

/* ── Colors ── */
#define COL_BG        lv_color_hex(0x1a1a2e)
#define COL_CARD      lv_color_hex(0x16213e)
#define COL_PRIMARY   lv_color_hex(0x0f3460)
#define COL_ACCENT    lv_color_hex(0xe94560)
#define COL_TEXT      lv_color_hex(0xeaeaea)
#define COL_TEXT_DIM  lv_color_hex(0x8899aa)
#define COL_GREEN     lv_color_hex(0x53d769)
#define COL_CYAN      lv_color_hex(0x4fc3f7)

/* ── Widget refs ── */
static lv_obj_t *lbl_track_title;
static lv_obj_t *lbl_track_artist;
static lv_obj_t *bar_progress;
static lv_obj_t *lbl_time;
static lv_obj_t *btn_play;
static lv_obj_t *lbl_play_icon;
static lv_obj_t *lbl_bt_status;
static lv_obj_t *list_tracks;
static lv_obj_t *lbl_track_count;
static lv_obj_t *btn_launcher;

static int s_selected_track = -1;

/* Forward declarations */
static void on_track_click(lv_event_t *e);
static void on_play_click(lv_event_t *e);
static void on_next_click(lv_event_t *e);
static void on_prev_click(lv_event_t *e);
static void on_bt_scan_click(lv_event_t *e);

static void return_to_launcher_task(void *arg)
{
    (void)arg;
    esp_err_t result = ota_loader_select_launcher();
    if (result == ESP_OK) {
        mp3_player_stop();
        bt_music_stop_stream();
        vTaskDelay(pdMS_TO_TICKS(50));
        esp_restart();
    }
    ESP_LOGE(TAG, "Return to launcher failed: %s", esp_err_to_name(result));
    lvgl_port_lock(0);
    lv_obj_remove_state(btn_launcher, LV_STATE_DISABLED);
    lv_obj_t *label = lv_obj_get_child(btn_launcher, 0);
    lv_label_set_text(label, "Back failed - retry");
    lvgl_port_unlock();
    vTaskDelete(NULL);
}

static void on_launcher_click(lv_event_t *e)
{
    (void)e;
    lv_obj_add_state(btn_launcher, LV_STATE_DISABLED);
    lv_obj_t *label = lv_obj_get_child(btn_launcher, 0);
    lv_label_set_text(label, "Returning...");
    if (xTaskCreate(return_to_launcher_task, "return_launcher", 8192, NULL, 5, NULL) != pdPASS) {
        lv_obj_remove_state(btn_launcher, LV_STATE_DISABLED);
        lv_label_set_text(label, "Not enough RAM - retry");
    }
}

/* ── Helpers ── */
static void format_time(uint32_t sec, char *buf, size_t len)
{
    snprintf(buf, len, "%d:%02d", (int)(sec / 60), (int)(sec % 60));
}

/* ── Update now-playing display ── */
static void update_now_playing(void)
{
    int idx = mp3_player_get_current_idx();
    if (mp3_player_get_state() == MP3_STATE_ERROR || (idx < 0 && mp3_player_get_error()[0])) {
        lv_label_set_text(lbl_track_title, "Playback unavailable");
        lv_label_set_text(lbl_track_artist, mp3_player_get_error());
        lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
        return;
    }
    if (idx < 0) {
        lv_label_set_text(lbl_track_title, "No track");
        lv_label_set_text(lbl_track_artist, "");
        lv_bar_set_value(bar_progress, 0, LV_ANIM_OFF);
        lv_label_set_text(lbl_time, "0:00 / 0:00");
        return;
    }

    const mp3_track_t *t = mp3_player_get_track(idx);
    if (t) {
        lv_label_set_text(lbl_track_title, t->title);
        lv_label_set_text(lbl_track_artist, t->artist[0] ? t->artist : t->name);
    }

    uint32_t elapsed = mp3_player_get_elapsed_sec();
    uint32_t duration = mp3_player_get_duration_sec();

    if (duration > 0) {
        lv_bar_set_value(bar_progress, (int32_t)((uint64_t)elapsed * 100 / duration), LV_ANIM_OFF);
    }

    char time_buf[32], elapsed_buf[12], duration_buf[12];
    format_time(elapsed, elapsed_buf, sizeof(elapsed_buf));
    format_time(duration, duration_buf, sizeof(duration_buf));
    snprintf(time_buf, sizeof(time_buf), "%s / %s", elapsed_buf, duration_buf);
    lv_label_set_text(lbl_time, time_buf);
}

/* ── Update play/pause button ── */
static void update_play_button(void)
{
    mp3_state_t state = mp3_player_get_state();
    if (state == MP3_STATE_PLAYING) {
        lv_label_set_text(lbl_play_icon, LV_SYMBOL_PAUSE);
    } else {
        lv_label_set_text(lbl_play_icon, LV_SYMBOL_PLAY);
    }
}

/* ── Update BT status ── */
static void update_bt_status(void)
{
    if (bt_music_is_connected()) {
        char name[64];
        bt_music_get_connected_name(name, sizeof(name));
        lv_label_set_text_fmt(lbl_bt_status, LV_SYMBOL_BLUETOOTH " %s", name);
        lv_obj_set_style_text_color(lbl_bt_status, COL_CYAN, 0);
    } else {
        lv_label_set_text(lbl_bt_status, LV_SYMBOL_BLUETOOTH " disconnected");
        lv_obj_set_style_text_color(lbl_bt_status, COL_TEXT_DIM, 0);
    }
}

/* ── Rebuild track list ── */
static void rebuild_track_list(void)
{
    lv_obj_clean(list_tracks);

    int count = mp3_player_track_count();
    lv_label_set_text_fmt(lbl_track_count, "%d tracks", count);

    for (int i = 0; i < count; i++) {
        const mp3_track_t *t = mp3_player_get_track(i);
        if (!t) continue;

        lv_obj_t *btn = lv_btn_create(list_tracks);
        lv_obj_set_size(btn, 218, 40);
        lv_obj_set_style_bg_color(btn, COL_CARD, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        lv_obj_add_event_cb(btn, on_track_click, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text_fmt(lbl, "%d. %s", i + 1, t->title);
        lv_obj_set_style_text_color(lbl, COL_TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_set_width(lbl, 200);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);

        /* Highlight current track */
        if (i == mp3_player_get_current_idx()) {
            lv_obj_set_style_bg_color(btn, COL_PRIMARY, 0);
        }
    }
}

/* ── Event callbacks ── */
static void on_track_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_selected_track = idx;
    mp3_player_play(idx);
    sleep_manager_activity();
}

static void on_play_click(lv_event_t *e)
{
    (void)e;
    mp3_state_t state = mp3_player_get_state();
    if (state == MP3_STATE_PLAYING) {
        mp3_player_pause();
    } else if (state == MP3_STATE_PAUSED) {
        mp3_player_resume();
    } else {
        /* Start playing first track if nothing selected */
        if (mp3_player_track_count() > 0) {
            mp3_player_play(s_selected_track >= 0 ? s_selected_track : 0);
        }
    }
    sleep_manager_activity();
}

static void on_next_click(lv_event_t *e)
{
    (void)e;
    mp3_player_next();
    sleep_manager_activity();
}

static void on_prev_click(lv_event_t *e)
{
    (void)e;
    mp3_player_prev();
    sleep_manager_activity();
}

/* ── BT scan/connect callbacks ── */

static void on_stop_click(lv_event_t *e)
{
    (void)e;
    mp3_player_stop();
    sleep_manager_activity();
}

static void scan_result_cb(const bt_device_info_t *dev, void *arg)
{
    (void)arg;
    /* No shared async argument: each scan result is applied under the UI lock. */
    if (lvgl_port_lock(50)) {
        bt_scan_ui_add_device(dev);
        lvgl_port_unlock();
    }
}

static void connect_state_cb(bt_conn_state_t state, const char *device_name, void *arg)
{
    (void)arg;
    ESP_LOGI(TAG, "BT state=%d name=%s", state, device_name ? device_name : "?");
    /* Status updates happen in the 500ms UI timer */
}

static void on_scan_connect(const bt_device_info_t *dev)
{
    ESP_LOGI(TAG, "Connecting to %s", dev->name);
    bt_scan_ui_set_status("Connecting...");
    bt_music_stop_scan();
    bt_music_connect(dev->addr, connect_state_cb, NULL);
}

static void on_bt_scan_click(lv_event_t *e)
{
    (void)e;
    ESP_LOGI(TAG, "Opening BT scan");
    bt_scan_ui_show(lv_screen_active(), on_scan_connect);
    bt_music_start_scan(scan_result_cb, NULL);
    bt_scan_ui_set_status("Scanning for devices...");
}

/* ── Timer to update UI ── */
static void ui_update_timer(lv_timer_t *timer)
{
    (void)timer;
    update_now_playing();
    update_play_button();
    update_bt_status();
}

/* ── Build UI ── */
void bt_music_ui_init(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, COL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    /* ── Now Playing card ── */
    lv_obj_t *card = lv_obj_create(scr);
    lv_obj_set_size(card, 232, 120);
    lv_obj_set_pos(card, 4, 12);
    lv_obj_set_style_bg_color(card, COL_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lbl_track_title = lv_label_create(card);
    lv_label_set_text(lbl_track_title, "No track");
    lv_obj_set_style_text_color(lbl_track_title, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl_track_title, &lv_font_montserrat_20, 0);
    lv_obj_set_width(lbl_track_title, 210);
    lv_label_set_long_mode(lbl_track_title, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_track_title, LV_ALIGN_TOP_LEFT, 0, 0);

    lbl_track_artist = lv_label_create(card);
    lv_label_set_text(lbl_track_artist, "");
    lv_obj_set_style_text_color(lbl_track_artist, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl_track_artist, &lv_font_montserrat_14, 0);
    lv_obj_set_width(lbl_track_artist, 210);
    lv_label_set_long_mode(lbl_track_artist, LV_LABEL_LONG_DOT);
    lv_obj_align(lbl_track_artist, LV_ALIGN_TOP_LEFT, 0, 28);

    bar_progress = lv_bar_create(card);
    lv_obj_set_size(bar_progress, 210, 8);
    lv_obj_align(bar_progress, LV_ALIGN_TOP_LEFT, 0, 52);
    lv_bar_set_range(bar_progress, 0, 100);
    lv_obj_set_style_bg_color(bar_progress, lv_color_hex(0x2a2a3e), LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar_progress, COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar_progress, 4, LV_PART_MAIN);
    lv_obj_set_style_radius(bar_progress, 4, LV_PART_INDICATOR);

    lbl_time = lv_label_create(card);
    lv_label_set_text(lbl_time, "0:00 / 0:00");
    lv_obj_set_style_text_color(lbl_time, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl_time, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_time, LV_ALIGN_TOP_LEFT, 0, 64);

    /* ── Playback controls ── */
    lv_obj_t *ctrl_row = lv_obj_create(card);
    lv_obj_set_size(ctrl_row, 210, 36);
    lv_obj_align(ctrl_row, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(ctrl_row, 0, 0);

    /* Prev */
    lv_obj_t *btn_prev = lv_btn_create(ctrl_row);
    lv_obj_set_size(btn_prev, 40, 32);
    lv_obj_align(btn_prev, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_prev, COL_PRIMARY, 0);
    lv_obj_set_style_radius(btn_prev, 8, 0);
    lv_obj_set_style_border_width(btn_prev, 0, 0);
    lv_obj_add_event_cb(btn_prev, on_prev_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_PREV);
    lv_obj_set_style_text_color(lbl_prev, COL_TEXT, 0);
    lv_obj_center(lbl_prev);

    /* Play/Pause */
    btn_play = lv_btn_create(ctrl_row);
    lv_obj_set_size(btn_play, 50, 32);
    lv_obj_center(btn_play);
    lv_obj_set_style_bg_color(btn_play, COL_ACCENT, 0);
    lv_obj_set_style_radius(btn_play, 8, 0);
    lv_obj_set_style_border_width(btn_play, 0, 0);
    lv_obj_add_event_cb(btn_play, on_play_click, LV_EVENT_CLICKED, NULL);

    lbl_play_icon = lv_label_create(btn_play);
    lv_label_set_text(lbl_play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_color(lbl_play_icon, lv_color_white(), 0);
    lv_obj_center(lbl_play_icon);

    /* Next */
    lv_obj_t *btn_next = lv_btn_create(ctrl_row);
    lv_obj_set_size(btn_next, 40, 32);
    lv_obj_align(btn_next, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(btn_next, COL_PRIMARY, 0);
    lv_obj_set_style_radius(btn_next, 8, 0);
    lv_obj_set_style_border_width(btn_next, 0, 0);
    lv_obj_add_event_cb(btn_next, on_next_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_NEXT);
    lv_obj_set_style_text_color(lbl_next, COL_TEXT, 0);
    lv_obj_center(lbl_next);

    /* ── BT status + scan ── */
    lbl_bt_status = lv_label_create(scr);
    lv_label_set_text(lbl_bt_status, LV_SYMBOL_BLUETOOTH " disconnected");
    lv_obj_set_style_text_color(lbl_bt_status, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl_bt_status, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_bt_status, LV_ALIGN_TOP_LEFT, 8, 138);

    lv_obj_t *btn_scan = lv_btn_create(scr);
    lv_obj_set_size(btn_scan, 55, 24);
    lv_obj_align(btn_scan, LV_ALIGN_TOP_RIGHT, -8, 136);
    lv_obj_set_style_bg_color(btn_scan, COL_PRIMARY, 0);
    lv_obj_set_style_radius(btn_scan, 6, 0);
    lv_obj_set_style_border_width(btn_scan, 0, 0);
    lv_obj_add_event_cb(btn_scan, on_bt_scan_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_scan = lv_label_create(btn_scan);
    lv_label_set_text(lbl_scan, "Scan");
    lv_obj_set_style_text_color(lbl_scan, COL_TEXT, 0);
    lv_obj_set_style_text_font(lbl_scan, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_scan);

    /* Stop transport button */
    lv_obj_t *btn_tone = lv_btn_create(scr);
    lv_obj_set_size(btn_tone, 55, 24);
    lv_obj_align(btn_tone, LV_ALIGN_TOP_RIGHT, -70, 136);
    lv_obj_set_style_bg_color(btn_tone, COL_ACCENT, 0);
    lv_obj_set_style_radius(btn_tone, 6, 0);
    lv_obj_set_style_border_width(btn_tone, 0, 0);
    lv_obj_add_event_cb(btn_tone, on_stop_click, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_tone = lv_label_create(btn_tone);
    lv_label_set_text(lbl_tone, LV_SYMBOL_STOP);
    lv_obj_set_style_text_color(lbl_tone, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_tone, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_tone);

    /* ── Track list header ── */
    lbl_track_count = lv_label_create(scr);
    lv_label_set_text(lbl_track_count, "0 tracks");
    lv_obj_set_style_text_color(lbl_track_count, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(lbl_track_count, &lv_font_montserrat_14, 0);
    lv_obj_align(lbl_track_count, LV_ALIGN_TOP_LEFT, 8, 165);

    /* ── Track list ── */
    list_tracks = lv_obj_create(scr);
    lv_obj_set_size(list_tracks, 232, 92);
    lv_obj_set_pos(list_tracks, 4, 182);
    lv_obj_set_style_bg_opa(list_tracks, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_tracks, 0, 0);
    lv_obj_set_style_pad_all(list_tracks, 2, 0);
    lv_obj_set_style_pad_row(list_tracks, 4, 0);
    lv_obj_set_flex_flow(list_tracks, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(list_tracks, LV_SCROLLBAR_MODE_AUTO);

    /* Always-visible navigation, separate from playback controls. */
    btn_launcher = lv_button_create(scr);
    lv_obj_set_size(btn_launcher, 224, 32);
    lv_obj_align(btn_launcher, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_set_style_bg_color(btn_launcher, COL_PRIMARY, 0);
    lv_obj_set_style_border_width(btn_launcher, 0, 0);
    lv_obj_set_style_radius(btn_launcher, 8, 0);
    lv_obj_add_event_cb(btn_launcher, on_launcher_click, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(btn_launcher);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT " Back to Launcher");
    lv_obj_set_style_text_color(back_label, COL_TEXT, 0);
    lv_obj_center(back_label);

    /* Initial state */
    rebuild_track_list();
    update_now_playing();
    update_play_button();
    update_bt_status();

    /* Update timer */
    lv_timer_create(ui_update_timer, 500, NULL);

    ESP_LOGI(TAG, "BT Music UI initialized");
}
