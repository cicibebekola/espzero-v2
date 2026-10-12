#include "ota_manager.h"
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "sd_card_manager.h"
#include "ota_loader.h"
#include "device_paths.h"
#include "launcher_dialogs.h"
#include "esp_system.h"
#include "file_server.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

static const char *TAG = "ota_mgr";

#define MAX_APPS 20
#define CARD_W   216
#define CARD_H   90
#define CARD_GAP 8
#define VISIBLE_APPS 2

/* Colors */
#define COL_BG       lv_color_hex(0x1a1a2e)
#define COL_CARD     lv_color_hex(0x16213e)
#define COL_ACCENT   lv_color_hex(0xe94560)
#define COL_GREEN    lv_color_hex(0x53d769)
#define COL_TEXT     lv_color_hex(0xeaeaea)
#define COL_TEXT_DIM lv_color_hex(0x8899aa)
#define COL_CYAN     lv_color_hex(0x4fc3f7)
#define COL_WARN     lv_color_hex(0xffcc00)

typedef struct {
    char     name[64];
    uint32_t size_kb;
    bool     valid;
} app_entry_t;

static app_entry_t s_apps[MAX_APPS];
static int s_app_count = 0;
static lv_obj_t *s_container = NULL;
static lv_obj_t *s_list = NULL;
static lv_obj_t *s_lbl_status = NULL;
static lv_obj_t *s_next_page;
static int s_page;
static atomic_bool s_flashing;
static char s_flash_path[256];
static char s_flash_name[64];
static int s_last_percent;

/* ── File size formatting ── */
static const char *format_size(uint32_t kb)
{
    static char buf[16];
    if (kb >= 1024) {
        snprintf(buf, sizeof(buf), "%.1f MB", kb / 1024.0f);
    } else {
        snprintf(buf, sizeof(buf), "%lu KB", (unsigned long)kb);
    }
    return buf;
}

/* ── Progress callback for OTA flash ── */
static void ota_progress_cb(size_t written, size_t total, void *arg)
{
    (void)arg;
    int percent = total ? (int)(written * 100 / total) : 0;
    if (percent != s_last_percent) {
        s_last_percent = percent;
        launcher_dialog_progress("Flashing firmware", s_flash_name, written, total);
    }
}

static void flash_task(void *arg)
{
    (void)arg;
    esp_err_t ret = ota_loader_flash_sd(s_flash_path, ota_progress_cb, NULL, false);
    if (ret == ESP_OK) {
        launcher_dialog_result("Flash complete", "Firmware verified.\nRestarting into the new app...", false);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    launcher_dialog_result("Flash failed", esp_err_to_name(ret), false);
    file_server_release_storage();
    s_flashing = false;
    vTaskDelete(NULL);
}

static void flash_confirmed(void *arg)
{
    int idx = (int)(intptr_t)arg;
    if (s_flashing || idx < 0 || idx >= s_app_count) return;
    if (!file_server_acquire_storage()) {
        launcher_dialog_message("Storage busy", "Wait for the web transfer to finish before flashing.");
        return;
    }
    snprintf(s_flash_name, sizeof(s_flash_name), "%s", s_apps[idx].name);
    snprintf(s_flash_path, sizeof(s_flash_path), DEVICE_APPS_DIR "/%s", s_flash_name);
    s_last_percent = -1;
    s_flashing = true;
    launcher_dialog_progress("Flashing firmware", s_flash_name, 0, 1);
    if (xTaskCreate(flash_task, "ota_flash", 6144, NULL, 5, NULL) != pdPASS) {
        s_flashing = false;
        file_server_release_storage();
        launcher_dialog_result("Flash failed", "Not enough memory to start flashing.", false);
    }
}

/* ── Flash button click ── */
static void on_flash_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_app_count) return;

    if (s_flashing || launcher_dialog_busy()) return;
    if (!sd_is_mounted()) {
        launcher_dialog_message("No SD card", "Insert an SD card and restart the launcher.");
        return;
    }
    char message[128];
    snprintf(message, sizeof(message), "Install and launch\n%s?\nThe device will restart.", s_apps[idx].name);
    launcher_dialog_confirm("Launch app", message, flash_confirmed, (void *)(intptr_t)idx);
}

/* ── Delete button click ── */
static void delete_confirmed(void *arg)
{
    int idx = (int)(intptr_t)arg;
    if (idx < 0 || idx >= s_app_count) return;
    if (!file_server_acquire_storage()) {
        launcher_dialog_message("Storage busy", "Wait for the web transfer to finish before deleting.");
        return;
    }

    char path[256];
    snprintf(path, sizeof(path), DEVICE_APPS_DIR "/%s", s_apps[idx].name);

    ESP_LOGI(TAG, "Deleting %s", path);
    if (remove(path) != 0) {
        file_server_release_storage();
        launcher_dialog_message("Delete failed", "Could not remove the file from SD.");
        return;
    }
    file_server_release_storage();
    ota_manager_refresh();
    launcher_dialog_message("App deleted", "The firmware file was removed from SD.");
}

static void on_delete_click(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_flashing || idx < 0 || idx >= s_app_count) return;
    char message[128];
    snprintf(message, sizeof(message), "Delete %s from the SD card?", s_apps[idx].name);
    launcher_dialog_confirm("Delete app", message, delete_confirmed, (void *)(intptr_t)idx);
}

/* ── Scan SD card for .bin files ── */
static void scan_apps(void)
{
    s_app_count = 0;

    sd_file_t files[30];
    size_t count = 0;
    sd_list_files_struct(DEVICE_APPS_DIR, files, 30, &count);

    for (int i = 0; i < count && s_app_count < MAX_APPS; i++) {
        if (files[i].is_dir) continue;
        if (!strstr(files[i].name, ".bin")) continue;

        strncpy(s_apps[s_app_count].name, files[i].name, sizeof(s_apps[0].name) - 1);

        /* Get file size */
        char path[256];
        snprintf(path, sizeof(path), DEVICE_APPS_DIR "/%s", files[i].name);
        struct stat st;
        if (stat(path, &st) == 0) {
            s_apps[s_app_count].size_kb = st.st_size / 1024;
        } else {
            s_apps[s_app_count].size_kb = 0;
        }
        s_apps[s_app_count].valid = true;
        s_app_count++;
    }

    ESP_LOGI(TAG, "Found %d apps", s_app_count);
}

/* ── Build one app card ── */
static void build_card(lv_obj_t *parent, int idx)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x282828), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(0x2a3a4e), 0);
    lv_obj_set_style_pad_all(card, 8, 0);
    lv_obj_set_scrollable(card, false);

    /* App icon placeholder */
    lv_obj_t *icon = lv_label_create(card);
    lv_label_set_text(icon, LV_SYMBOL_FILE);
    lv_obj_set_style_text_color(icon, COL_CYAN, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_align(icon, LV_ALIGN_TOP_LEFT, 0, 0);

    /* App name */
    lv_obj_t *name = lv_label_create(card);
    lv_label_set_text(name, s_apps[idx].name);
    lv_obj_set_style_text_color(name, COL_TEXT, 0);
    lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
    lv_obj_set_width(name, 91);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_align(name, LV_ALIGN_TOP_LEFT, 32, 2);

    /* Size */
    lv_obj_t *size_lbl = lv_label_create(card);
    lv_label_set_text_fmt(size_lbl, "%s", format_size(s_apps[idx].size_kb));
    lv_obj_set_style_text_color(size_lbl, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(size_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(size_lbl, LV_ALIGN_TOP_LEFT, 32, 22);

    /* Flash button */
    lv_obj_t *btn_flash = lv_btn_create(card);
    lv_obj_set_size(btn_flash, 66, 32);
    lv_obj_align(btn_flash, LV_ALIGN_TOP_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_flash, COL_GREEN, 0);
    lv_obj_set_style_radius(btn_flash, 8, 0);
    lv_obj_set_style_border_width(btn_flash, 0, 0);
    lv_obj_add_event_cb(btn_flash, on_flash_click, LV_EVENT_CLICKED,
                        (void *)(intptr_t)idx);

    lv_obj_t *lbl_flash = lv_label_create(btn_flash);
    lv_label_set_text(lbl_flash, "Launch");
    lv_obj_set_style_text_color(lbl_flash, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_flash, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_flash);

    /* Delete button */
    lv_obj_t *btn_del = lv_btn_create(card);
    lv_obj_set_size(btn_del, 66, 32);
    lv_obj_align(btn_del, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_del, COL_ACCENT, 0);
    lv_obj_set_style_radius(btn_del, 8, 0);
    lv_obj_set_style_border_width(btn_del, 0, 0);
    lv_obj_add_event_cb(btn_del, on_delete_click, LV_EVENT_CLICKED,
                        (void *)(intptr_t)idx);

    lv_obj_t *lbl_del = lv_label_create(btn_del);
    lv_label_set_text(lbl_del, "Delete");
    lv_obj_set_style_text_color(lbl_del, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl_del, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl_del);
}

/* ── Build the full UI ── */
static void build_ui(void)
{
    /* Clear old content */
    lv_obj_clean(s_list);

    if (s_app_count == 0) {
        lv_obj_t *empty = lv_label_create(s_list);
        lv_label_set_text(empty, LV_SYMBOL_WARNING " No apps found\n\n"
                                "Place .bin files in /sdcard/apps/");
        lv_obj_set_style_text_color(empty, COL_TEXT_DIM, 0);
        lv_obj_set_style_text_font(empty, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(empty);
        lv_obj_set_hidden(s_next_page, true);
        return;
    }

    /* Two fixed app cards. This avoids a carousel on the small display. */
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, CARD_GAP, 0);
    lv_obj_set_scrollable(s_list, false);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    if (s_page * VISIBLE_APPS >= s_app_count) s_page = 0;
    int end = (s_page + 1) * VISIBLE_APPS;
    if (end > s_app_count) end = s_app_count;
    for (int i = s_page * VISIBLE_APPS; i < end; i++) {
        build_card(s_list, i);
    }
    lv_obj_set_hidden(s_next_page, s_app_count <= VISIBLE_APPS);
    lv_label_set_text_fmt(s_lbl_status, "%d apps  |  Page %d/%d", s_app_count, s_page + 1,
                         (s_app_count + VISIBLE_APPS - 1) / VISIBLE_APPS);
}

static void next_page(lv_event_t *event)
{
    (void)event;
    if (s_flashing || s_app_count == 0) return;
    int pages = (s_app_count + VISIBLE_APPS - 1) / VISIBLE_APPS;
    s_page = (s_page + 1) % pages;
    build_ui();
}

/* ── Public API ── */

void ota_manager_init(lv_obj_t *parent)
{
    s_container = parent;

    /* Status label only (no title) */
    s_lbl_status = lv_label_create(parent);
    lv_label_set_text(s_lbl_status, "Scanning...");
    lv_obj_set_style_text_color(s_lbl_status, COL_TEXT_DIM, 0);
    lv_obj_set_style_text_font(s_lbl_status, &lv_font_montserrat_14, 0);
    lv_obj_align(s_lbl_status, LV_ALIGN_TOP_LEFT, 4, 0);

    /* Fixed two-card app list */
    s_list = lv_obj_create(parent);
    lv_obj_set_size(s_list, 228, 208);
    lv_obj_set_pos(s_list, 0, 22);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 4, 0);
    lv_obj_set_style_pad_row(s_list, 0, 0);
    lv_obj_set_scrollable(s_list, false);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    s_next_page = lv_button_create(parent);
    lv_obj_set_size(s_next_page, 120, 36);
    lv_obj_align(s_next_page, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_style_bg_color(s_next_page, lv_color_hex(0x484848), 0);
    lv_obj_t *next_label = lv_label_create(s_next_page);
    lv_label_set_text(next_label, "Next page");
    lv_obj_center(next_label);
    lv_obj_add_event_cb(s_next_page, next_page, LV_EVENT_CLICKED, NULL);

    /* Initial scan */
    scan_apps();
    lv_label_set_text_fmt(s_lbl_status, "%d apps", s_app_count);
    build_ui();

    ESP_LOGI(TAG, "OTA manager initialized (%d apps)", s_app_count);
}

void ota_manager_refresh(void)
{
    if (!s_list || s_flashing) return;
    scan_apps();
    lv_label_set_text_fmt(s_lbl_status, "%d apps", s_app_count);
    build_ui();
}
