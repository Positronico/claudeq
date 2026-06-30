// Speaker output (ES8311 DAC) + microphone capture (ES7210 ADC) on a shared full-duplex I2S0.
// Output: plays PCM clips streamed from the bridge (binary WS frames) + synth fallback tones.
// Input: on hold-to-talk, streams mic PCM to the bridge as binary WS frames.
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2s_std.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "app.h"

static const char *TAG = "audio";

#define I2S_MCLK 7
#define I2S_BCLK 15
#define I2S_WS   46
#define I2S_DOUT 45   // -> ES8311 (speaker)
#define I2S_DIN  6    // <- ES7210 (mics)
#define SR       16000

enum { SND_CHIRP, SND_SOFT, SND_ERROR, SND_BEEP };
typedef struct { uint8_t kind; int tone; void *pcm; size_t bytes; } play_req_t;  // kind 0=tone, 1=pcm

static esp_codec_dev_handle_t s_dev = NULL;          // ES8311 playback
static esp_codec_dev_handle_t s_record_dev = NULL;   // ES7210 capture
static i2s_chan_handle_t s_rx = NULL;
static QueueHandle_t s_q = NULL;                      // playback queue
static int16_t *s_buf = NULL;                        // synth tone buffer
#define MAX_FRAMES 16000
static volatile bool s_recording = false;
static TaskHandle_t s_rec_task = NULL;

// ---------- synth tones (fallback) ----------
static int gen_tone(int frame_off, float freq, int ms, float amp) {
    int n = SR * ms / 1000;
    if (frame_off + n > MAX_FRAMES) n = MAX_FRAMES - frame_off;
    int fade = SR * 10 / 1000;
    for (int i = 0; i < n; i++) {
        float env = 1.0f;
        if (i < fade) env = (float)i / fade;
        else if (i > n - fade) env = (float)(n - i) / fade;
        s_buf[frame_off + i] = (int16_t)(amp * 32767.0f * env * sinf(2.0f * (float)M_PI * freq * i / SR));
    }
    return frame_off + n;
}
static void play_sound(int id) {
    int f = 0;
    switch (id) {
        case SND_CHIRP: f = gen_tone(f, 660, 200, 0.5f);  f = gen_tone(f, 0, 130, 0.0f); f = gen_tone(f, 988, 260, 0.5f); break;
        case SND_SOFT:  f = gen_tone(f, 784, 320, 0.45f); break;
        case SND_ERROR: f = gen_tone(f, 392, 220, 0.55f); f = gen_tone(f, 0, 130, 0.0f); f = gen_tone(f, 294, 300, 0.55f); break;
        default:        f = gen_tone(f, 784, 260, 0.45f); break;
    }
    if (f <= 0 || !s_dev) return;
    esp_codec_dev_write(s_dev, s_buf, f * sizeof(int16_t));
    vTaskDelay(pdMS_TO_TICKS(f * 1000 / SR + 60));
}

// ---------- playback task ----------
static void audio_task(void *arg) {
    play_req_t r;
    for (;;) {
        if (xQueueReceive(s_q, &r, portMAX_DELAY) != pdTRUE) continue;
        if (r.kind == 0) {
            play_sound(r.tone);
        } else if (r.kind == 1 && r.pcm) {
            if (s_dev) esp_codec_dev_write(s_dev, r.pcm, r.bytes);
            vTaskDelay(pdMS_TO_TICKS((r.bytes / 2) * 1000 / SR + 60));
            heap_caps_free(r.pcm);
        }
    }
}

extern "C" void audio_play_pcm(const void *data, size_t bytes) {
    if (!s_dev || !data || bytes == 0 || bytes > 1024 * 1024) return;
    void *copy = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!copy) { ESP_LOGW(TAG, "pcm alloc failed (%u)", (unsigned)bytes); return; }
    memcpy(copy, data, bytes);
    play_req_t r = { 1, 0, copy, bytes };
    if (!s_q || xQueueSend(s_q, &r, 0) != pdTRUE) heap_caps_free(copy);
}
extern "C" void audio_play_alert(const char *sound) {
    play_req_t r = { 0, SND_BEEP, NULL, 0 };
    if (sound) {
        if (!strcmp(sound, "chirp")) r.tone = SND_CHIRP;
        else if (!strcmp(sound, "soft")) r.tone = SND_SOFT;
        else if (!strcmp(sound, "error")) r.tone = SND_ERROR;
    }
    if (s_q) xQueueSend(s_q, &r, 0);
}

// ---------- mic capture (hold-to-talk) ----------
static void record_task(void *arg) {
    const int CHUNK = 1024;                       // 512 samples = 32 ms @16k mono
    uint8_t *buf = (uint8_t *)malloc(CHUNK);
    if (!buf) { vTaskDelete(NULL); return; }
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);  // wait until a capture begins
        ESP_LOGI(TAG, "mic capture start");
        while (s_recording && s_record_dev) {
            if (esp_codec_dev_read(s_record_dev, buf, CHUNK) == ESP_CODEC_DEV_OK) {
                net_send_binary(buf, CHUNK);
            } else {
                vTaskDelay(pdMS_TO_TICKS(5));
            }
        }
        ESP_LOGI(TAG, "mic capture stop");
    }
}
extern "C" void audio_record_start(void) { if (!s_record_dev) return; s_recording = true; if (s_rec_task) xTaskNotifyGive(s_rec_task); }
extern "C" void audio_record_stop(void)  { s_recording = false; }

// ---------- init ----------
extern "C" void audio_init(void) {
    i2c_master_bus_handle_t i2c_bus = NULL;
    if (i2c_master_get_bus_handle(0, &i2c_bus) != ESP_OK || !i2c_bus) { ESP_LOGE(TAG, "no i2c bus 0"); return; }

    // One full-duplex I2S0: tx (ES8311) + rx (ES7210), shared clocks.
    i2s_chan_handle_t tx = NULL;
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.auto_clear = true;
    if (i2s_new_channel(&cc, &tx, &s_rx) != ESP_OK) { ESP_LOGE(TAG, "i2s_new_channel"); return; }
    i2s_std_config_t std = {};
    std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SR);
    std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
    std.gpio_cfg.mclk = (gpio_num_t)I2S_MCLK;
    std.gpio_cfg.bclk = (gpio_num_t)I2S_BCLK;
    std.gpio_cfg.ws   = (gpio_num_t)I2S_WS;
    std.gpio_cfg.dout = (gpio_num_t)I2S_DOUT;
    std.gpio_cfg.din  = (gpio_num_t)I2S_DIN;
    if (i2s_channel_init_std_mode(tx, &std) != ESP_OK)   { ESP_LOGE(TAG, "i2s std init tx"); return; }
    if (i2s_channel_init_std_mode(s_rx, &std) != ESP_OK) { ESP_LOGE(TAG, "i2s std init rx"); return; }

    audio_codec_i2s_cfg_t i2s_cfg = {};
    i2s_cfg.port = I2S_NUM_0; i2s_cfg.tx_handle = tx; i2s_cfg.rx_handle = s_rx;
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);  // shared by play + record
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();

    // ES8311 playback (DAC)
    audio_codec_i2c_cfg_t spk_i2c = {};
    spk_i2c.port = 0; spk_i2c.addr = ES8311_CODEC_DEFAULT_ADDR; spk_i2c.bus_handle = i2c_bus;
    const audio_codec_ctrl_if_t *spk_ctrl = audio_codec_new_i2c_ctrl(&spk_i2c);
    es8311_codec_cfg_t es = {};
    es.codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC;
    es.ctrl_if = spk_ctrl; es.gpio_if = gpio_if;
    es.pa_pin = -1; es.use_mclk = true; es.hw_gain.pa_gain = 6.0f;
    const audio_codec_if_t *spk_codec = es8311_codec_new(&es);
    if (!spk_codec) { ESP_LOGE(TAG, "es8311_codec_new failed"); return; }
    esp_codec_dev_cfg_t play_cfg = {};
    play_cfg.dev_type = ESP_CODEC_DEV_TYPE_OUT; play_cfg.codec_if = spk_codec; play_cfg.data_if = data_if;
    s_dev = esp_codec_dev_new(&play_cfg);
    if (!s_dev) { ESP_LOGE(TAG, "esp_codec_dev_new(out) failed"); return; }
    esp_codec_dev_sample_info_t fs = {};
    fs.bits_per_sample = 16; fs.channel = 1; fs.sample_rate = SR;
    if (esp_codec_dev_open(s_dev, &fs) != ESP_OK) { ESP_LOGE(TAG, "spk open failed"); return; }
    esp_codec_dev_set_out_vol(s_dev, 90.0f);
    app_io_set_pa(true);

    // ES7210 capture (ADC) on the SAME data_if
    audio_codec_i2c_cfg_t mic_i2c = {};
    mic_i2c.port = 0; mic_i2c.addr = ES7210_CODEC_DEFAULT_ADDR; mic_i2c.bus_handle = i2c_bus;
    const audio_codec_ctrl_if_t *mic_ctrl = audio_codec_new_i2c_ctrl(&mic_i2c);
    es7210_codec_cfg_t mic = {};
    mic.ctrl_if = mic_ctrl;
    mic.mic_selected = ES7120_SEL_MIC1;
    const audio_codec_if_t *mic_codec = es7210_codec_new(&mic);
    if (mic_codec) {
        esp_codec_dev_cfg_t rec_cfg = {};
        rec_cfg.dev_type = ESP_CODEC_DEV_TYPE_IN; rec_cfg.codec_if = mic_codec; rec_cfg.data_if = data_if;
        s_record_dev = esp_codec_dev_new(&rec_cfg);
        esp_codec_dev_sample_info_t rfs = {};
        rfs.bits_per_sample = 16; rfs.channel = 1; rfs.sample_rate = SR;
        if (s_record_dev && esp_codec_dev_open(s_record_dev, &rfs) == ESP_OK) {
            esp_codec_dev_set_in_gain(s_record_dev, 32.0f);
            xTaskCreate(record_task, "mic", 4096, NULL, 5, &s_rec_task);
            ESP_LOGI(TAG, "ES7210 mic ready");
        } else { ESP_LOGE(TAG, "mic open failed"); s_record_dev = NULL; }
    } else { ESP_LOGE(TAG, "es7210_codec_new failed"); }

    s_buf = (int16_t *)malloc(MAX_FRAMES * sizeof(int16_t));
    s_q = xQueueCreate(6, sizeof(play_req_t));
    if (!s_buf || !s_q) { ESP_LOGE(TAG, "alloc failed"); return; }
    xTaskCreate(audio_task, "audio", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "audio ready (ES8311 out + ES7210 in, mono @16k)");
}
