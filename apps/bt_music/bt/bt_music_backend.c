#include "bt_music_backend.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"

#include <math.h>

#include "esp_log.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_a2dp_api.h"
#include "esp_avrc_api.h"
#include "nvs_flash.h"

#define TAG "BT_MUSIC"

/* ~46 ms at 44.1 kHz stereo; producers send bounded 1 KB chunks. */
#define PCM_RINGBUF_SIZE  (8 * 1024)

static RingbufHandle_t      s_pcm_ringbuf  = NULL;
static bt_conn_state_t      s_state        = BT_STATE_IDLE;
static char                 s_dev_name[BT_DEVICE_NAME_MAX] = {0};
static uint8_t              s_volume       = 80;
static bool                 s_inited       = false;
static volatile bool        s_streaming    = false;   /* true between START and STOP */
static volatile bool        s_stream_requested = false;
static volatile int         s_sample_rate = 44100;
static bool                 s_tone_active  = false;   /* feed sine tone when no audio source */
static uint32_t             s_tone_phase   = 0;       /* sine phase accumulator */

static bt_scan_result_cb_t  s_scan_cb  = NULL;
static void                *s_scan_arg = NULL;
static bt_conn_state_cb_t   s_conn_cb  = NULL;
static void                *s_conn_arg = NULL;
static uint8_t              s_peer_bda[6] = {0};

/* Pending-name table: devices seen during inquiry that had no name yet.
 * We fire esp_bt_gap_read_remote_name() for them and store addr+rssi here
 * so the READ_REMOTE_NAME_EVT handler can build the full bt_device_info_t. */
#define PENDING_MAX 4
typedef struct {
    uint8_t addr[6];
    int8_t  rssi;
    bool    used;
} pending_dev_t;
static pending_dev_t s_pending[PENDING_MAX];

/* =========================================================
 * A2DP source data callback — called by BT stack when it
 * needs PCM data to encode and transmit.
 * ========================================================= */

/* 440 Hz sine tone at 44100 Hz sample rate, stereo int16 */
#define TONE_HZ       440
#define TONE_SAMPLE_RATE 44100
#define TONE_AMPLITUDE   8000  /* ~25% of full scale to be gentle on ears */

static void fill_sine_tone(uint8_t *buf, int32_t len)
{
    int16_t *out = (int16_t *)buf;
    int samples = len / 2;
    for (int i = 0; i < samples; i += 2) {
        // High-performance square wave alternative (Zero floating point math)
        int16_t val = (s_tone_phase < 32768) ? TONE_AMPLITUDE : -TONE_AMPLITUDE;
        out[i]   = val; // L
        out[i+1] = val; // R
        s_tone_phase = (s_tone_phase + (TONE_HZ * 65536 / TONE_SAMPLE_RATE)) & 0xFFFF;
    }
}

static int32_t a2dp_data_cb(uint8_t *buf, int32_t len)
{
    if (!s_pcm_ringbuf || len <= 0)
        return 0;
    if (!s_stream_requested) { memset(buf, 0, len); return len; }

    /* Tone generator takes priority for testing without audio source */
    if (s_tone_active) {
        fill_sine_tone(buf, len);
        return len;
    }

    size_t available = 0;
    uint8_t *data = (uint8_t *)xRingbufferReceiveUpTo(
                        s_pcm_ringbuf, &available,
                        0,              /* don't block */
                        (size_t)len);

    if (!data || available == 0) {
        /* Underrun — send silence */
        memset(buf, 0, len);
        return len;
    }

    memcpy(buf, data, available);
    vRingbufferReturnItem(s_pcm_ringbuf, data);

    if ((int32_t)available < len)
        memset(buf + available, 0, len - available);

    return len;
}

/* =========================================================
 * A2DP source event callback
 * ========================================================= */

static void a2dp_cb(esp_a2d_cb_event_t event, esp_a2d_cb_param_t *param)
{
    switch (event) {
    case ESP_A2D_AUDIO_CFG_EVT:
        if (param->audio_cfg.mcc.type == ESP_A2D_MCT_SBC) {
            uint8_t frequency = param->audio_cfg.mcc.cie.sbc_info.samp_freq;
            s_sample_rate = frequency & ESP_A2D_SBC_CIE_SF_48K ? 48000 :
                            frequency & ESP_A2D_SBC_CIE_SF_44K ? 44100 :
                            frequency & ESP_A2D_SBC_CIE_SF_32K ? 32000 : 16000;
            ESP_LOGI(TAG, "Bluetooth PCM: %d Hz", s_sample_rate);
        }
        break;
    case ESP_A2D_CONNECTION_STATE_EVT: {
        esp_a2d_cb_param_t *p = param;
        if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            s_state = BT_STATE_CONNECTED;
            s_streaming = false;
            ESP_LOGI(TAG, "A2DP connected");
        } else if (p->conn_stat.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            s_state = BT_STATE_DISCONNECTED;
            s_streaming = false;
            s_stream_requested = false;
            s_tone_active = false;
            memset(s_peer_bda, 0, 6);
            ESP_LOGI(TAG, "A2DP disconnected");
            if (s_conn_cb)
                s_conn_cb(BT_STATE_DISCONNECTED, s_dev_name, s_conn_arg);
        }
        break;
    }
    case ESP_A2D_AUDIO_STATE_EVT:
        if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_STARTED) {
            s_streaming = true;
            ESP_LOGI(TAG, "A2DP audio started");
            if (s_conn_cb)
                s_conn_cb(BT_STATE_CONNECTED, s_dev_name, s_conn_arg);
        } else if (param->audio_stat.state == ESP_A2D_AUDIO_STATE_SUSPEND) {
            s_streaming = false;
        }
        break;
    case ESP_A2D_MEDIA_CTRL_ACK_EVT:
        if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY &&
            param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS && s_stream_requested) {
            esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_START);
        } else if (param->media_ctrl_stat.cmd == ESP_A2D_MEDIA_CTRL_SUSPEND &&
                   param->media_ctrl_stat.status == ESP_A2D_MEDIA_CTRL_ACK_SUCCESS) {
            s_streaming = false;
            /* A new track/resume may arrive while the previous suspend is
             * still asynchronous. Restart only if audio is wanted now. */
            if (s_stream_requested) esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        }
        break;
    default:
        break;
    }
}

/* =========================================================
 * GAP callback — handles scan results and connection events
 * ========================================================= */

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        bt_device_info_t dev = {0};
        memcpy(dev.addr, param->disc_res.bda, 6);
        dev.rssi = -80;

        for (int i = 0; i < param->disc_res.num_prop; i++) {
            esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
            if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME && p->len > 0) {
                size_t copy = p->len < BT_DEVICE_NAME_MAX - 1
                              ? p->len : BT_DEVICE_NAME_MAX - 1;
                memcpy(dev.name, p->val, copy);
                dev.name[copy] = '\0';
            } else if (p->type == ESP_BT_GAP_DEV_PROP_RSSI) {
                dev.rssi = *(int8_t *)p->val;
            }
        }

        if (dev.name[0] != '\0') {
            /* Name arrived with the inquiry result (EIR) — report immediately */
            ESP_LOGI(TAG, "Found: %s  RSSI=%d", dev.name, dev.rssi);
            if (s_scan_cb)
                s_scan_cb(&dev, s_scan_arg);
        } else {
            /* No name yet — store addr+rssi and request it explicitly */
            for (int i = 0; i < PENDING_MAX; i++) {
                if (!s_pending[i].used) {
                    memcpy(s_pending[i].addr, dev.addr, 6);
                    s_pending[i].rssi = dev.rssi;
                    s_pending[i].used = true;
                    esp_bt_gap_read_remote_name(dev.addr);
                    break;
                }
            }
        }
        break;
    }

    case ESP_BT_GAP_READ_REMOTE_NAME_EVT: {
        if (param->read_rmt_name.stat != ESP_BT_STATUS_SUCCESS)
            break;
        /* Find the stored pending entry for this address */
        for (int i = 0; i < PENDING_MAX; i++) {
            if (s_pending[i].used &&
                memcmp(s_pending[i].addr, param->read_rmt_name.bda, 6) == 0) {
                bt_device_info_t dev = {0};
                memcpy(dev.addr, s_pending[i].addr, 6);
                dev.rssi = s_pending[i].rssi;
                strncpy(dev.name, (char *)param->read_rmt_name.rmt_name,
                        BT_DEVICE_NAME_MAX - 1);
                s_pending[i].used = false;
                ESP_LOGI(TAG, "Found (rmt): %s  RSSI=%d", dev.name, dev.rssi);
                if (s_scan_cb)
                    s_scan_cb(&dev, s_scan_arg);
                break;
            }
        }
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "Scan stopped");
            memset(s_pending, 0, sizeof(s_pending));
            if (s_state == BT_STATE_SCANNING)
                s_state = BT_STATE_IDLE;
        }
        break;

    case ESP_BT_GAP_AUTH_CMPL_EVT:
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "Auth OK: %s", param->auth_cmpl.device_name);
            strncpy(s_dev_name, (char *)param->auth_cmpl.device_name,
                    BT_DEVICE_NAME_MAX - 1);
        } else {
            ESP_LOGW(TAG, "Auth failed: %d", param->auth_cmpl.stat);
        }
        break;

    case ESP_BT_GAP_PIN_REQ_EVT: {
        uint8_t pin[ESP_BT_PIN_CODE_LEN] = {'1','2','3','4'};
        esp_bt_gap_pin_reply(param->pin_req.bda, true, 4, pin);
        break;
    }

    case ESP_BT_GAP_CFM_REQ_EVT:
        ESP_LOGI(TAG, "SSP confirm: num_val=%lu", (unsigned long)param->cfm_req.num_val);
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, true);
        break;

    case ESP_BT_GAP_KEY_NOTIF_EVT:
        ESP_LOGI(TAG, "SSP passkey: %06d", param->key_notif.passkey);
        break;

    case ESP_BT_GAP_READ_RSSI_DELTA_EVT:
        break;

    default:
        break;
    }
}

/* =========================================================
 * AVRCP target callback (volume sync from headphone)
 * ========================================================= */

static void avrcp_tg_cb(esp_avrc_tg_cb_event_t event, esp_avrc_tg_cb_param_t *param)
{
    if (event == ESP_AVRC_TG_SET_ABSOLUTE_VOLUME_CMD_EVT) {
        s_volume = param->set_abs_vol.volume;
        ESP_LOGI(TAG, "AVRCP volume set: %d", s_volume);
    }
}

/* =========================================================
 * Public API
 * ========================================================= */

esp_err_t bt_music_init(void)
{
    if (s_inited)
        return ESP_OK;

    s_pcm_ringbuf = xRingbufferCreate(PCM_RINGBUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_pcm_ringbuf) {
        ESP_LOGE(TAG, "Failed to create PCM ring buffer");
        return ESP_ERR_NO_MEM;
    }

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    ESP_ERROR_CHECK(esp_bluedroid_init());
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));

    /* Enable SSP with no-input-no-output (auto-accept) */
    esp_bt_sp_param_t param_type = ESP_BT_SP_IOCAP_MODE;
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    esp_bt_gap_set_security_param(param_type, &iocap, sizeof(iocap));

    /* AVRCP must exist before A2DP source init (required by Bluedroid). */
    ESP_ERROR_CHECK(esp_avrc_tg_init());
    ESP_ERROR_CHECK(esp_avrc_tg_register_callback(avrcp_tg_cb));
    ESP_ERROR_CHECK(esp_a2d_register_callback(a2dp_cb));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(a2dp_data_cb));
    ESP_ERROR_CHECK(esp_a2d_source_init());


    /* Stay connectable + discoverable so swift-connect works immediately */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    esp_bt_gap_set_device_name("EspZero Music");

    s_state   = BT_STATE_IDLE;
    s_inited  = true;
    ESP_LOGI(TAG, "BT init OK");
    return ESP_OK;
}

void bt_music_start_passive(void)
{
    /* Already connectable/discoverable from init; just ensure scan mode is set. */
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
}

void bt_music_stop_passive(void)
{
    esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
}

void bt_music_deinit(void)
{
    if (!s_inited)
        return;

    bt_music_stop_scan();
    bt_music_disconnect();

    esp_a2d_source_deinit();
    esp_avrc_tg_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    if (s_pcm_ringbuf) {
        vRingbufferDelete(s_pcm_ringbuf);
        s_pcm_ringbuf = NULL;
    }

    s_inited = false;
}

esp_err_t bt_music_start_scan(bt_scan_result_cb_t cb, void *arg)
{
    s_scan_cb  = cb;
    s_scan_arg = arg;
    s_state    = BT_STATE_SCANNING;
    memset(s_pending, 0, sizeof(s_pending));
    /* 10 × 1.28s = ~13s inquiry window, unlimited results */
    return esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, 10, 0);
}

void bt_music_stop_scan(void)
{
    esp_bt_gap_cancel_discovery();
    s_scan_cb = NULL;
    if (s_state == BT_STATE_SCANNING)
        s_state = BT_STATE_IDLE;
}

esp_err_t bt_music_connect(const uint8_t addr[6], bt_conn_state_cb_t cb, void *arg)
{
    s_conn_cb  = cb;
    s_conn_arg = arg;
    s_state    = BT_STATE_CONNECTING;
    memcpy(s_peer_bda, addr, 6);
    bt_music_stop_scan();
    esp_err_t ret = esp_a2d_source_connect((uint8_t *)addr);
    if (ret != ESP_OK) {
        s_state = BT_STATE_IDLE;
        memset(s_peer_bda, 0, 6);
    }
    return ret;
}

void bt_music_disconnect(void)
{
    if (s_state == BT_STATE_CONNECTED || s_state == BT_STATE_CONNECTING)
        esp_a2d_source_disconnect(s_peer_bda);
    memset(s_peer_bda, 0, 6);
    s_state = BT_STATE_IDLE;
}

void bt_music_start_stream(void)
{
    s_stream_requested = true;
    if (s_state == BT_STATE_CONNECTED && !s_streaming)
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
}

void bt_music_stop_stream(void)
{
    s_stream_requested = false;
    s_tone_active = false;
    if (s_streaming) {
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_SUSPEND);
        s_tone_active = false;
    }
}

void bt_music_set_tone(bool enable)
{
    s_tone_active = enable;
    s_tone_phase  = 0;
    if (enable)
        bt_music_start_stream();
}

bool bt_music_is_connected(void)
{
    return s_state == BT_STATE_CONNECTED;
}

bt_conn_state_t bt_music_get_state(void)
{
    return s_state;
}

void bt_music_get_connected_name(char *buf, size_t len)
{
    strncpy(buf, s_dev_name, len - 1);
    buf[len - 1] = '\0';
}

bool bt_music_feed_pcm(const int16_t *pcm, size_t frames)
{
    if (!s_pcm_ringbuf || !bt_music_is_connected() || frames > 256)
        return false;
    /* frames = number of stereo sample pairs; each is 4 bytes */
    return xRingbufferSend(s_pcm_ringbuf, (void *)pcm,
                    frames * 2 * sizeof(int16_t),
                    pdMS_TO_TICKS(20)) == pdTRUE;
}

void bt_music_clear_pcm(void)
{
    if (!s_pcm_ringbuf) return;
    size_t size; void *data;
    while ((data = xRingbufferReceiveUpTo(s_pcm_ringbuf, &size, 0, PCM_RINGBUF_SIZE)))
        vRingbufferReturnItem(s_pcm_ringbuf, data);
}

size_t bt_music_pending_pcm(void)
{
    if (!s_pcm_ringbuf) return 0;
    UBaseType_t waiting = 0;
    vRingbufferGetInfo(s_pcm_ringbuf, NULL, NULL, NULL, NULL, &waiting);
    return waiting;
}

int bt_music_sample_rate(void) { return s_sample_rate; }

void bt_music_set_volume(uint8_t vol)
{
    s_volume = vol > 127 ? 127 : vol;
    if (bt_music_is_connected()) {
        esp_avrc_rn_param_t param = { .volume = s_volume };
        esp_avrc_tg_send_rn_rsp(ESP_AVRC_RN_VOLUME_CHANGE,
                                 ESP_AVRC_RN_RSP_CHANGED, &param);
    }
}

uint8_t bt_music_get_volume(void)
{
    return s_volume;
}
