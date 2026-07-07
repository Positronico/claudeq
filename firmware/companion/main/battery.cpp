// Battery / system-voltage sense on the ESP32-S3-Touch-LCD-3.49 (V2).
// BAT_ADC = GPIO4 = ADC1 channel 3, behind a 3:1 divider, so V_sys = measured * 3 (per Waveshare's example).
// V_sys tracks the 1S Li-ion when unplugged and is pulled up (~4.4V+) when USB power is present, which we use
// as a rough "charging / on USB" flag. The percentage is an approximate resting-voltage curve (good enough for
// a glanceable indicator, not fuel-gauge accurate — there's no BQ27220 populated on this board).
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "app.h"

static const char *TAG = "battery";

#define BAT_ADC_UNIT    ADC_UNIT_1
#define BAT_ADC_CHANNEL ADC_CHANNEL_3     // GPIO4
#define BAT_DIVIDER     3.0f              // on-board resistor divider ratio
#define CHARGING_VOLTS  4.35f             // (unused in practice) this board reads VBAT, which stays ~4.14-4.18V
                                          // whether charging or resting near-full, so charging isn't detectable
                                          // by voltage; SYS_OUT (GPIO16) is also constant. Flag stays false.

static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;   // NULL -> fall back to the raw 3.3V/4096 conversion
static bool s_ready = false;

// Resting 1S Li-ion discharge curve (volts -> percent), high to low. Interpolated between points.
// Saturates to 100% at ~4.15V: a battery resting there has essentially finished charging (a Li-ion only
// hits 4.20V under active charge and settles back), so it should read full rather than ~96%.
static const struct { float v; int pct; } CURVE[] = {
    {4.15f,100},{4.08f,92},{4.00f,84},{3.90f,70},{3.85f,62},{3.80f,54},
    {3.75f,46},{3.70f,38},{3.65f,29},{3.60f,20},{3.50f,10},{3.40f,5},{3.30f,0},
};

static int volts_to_pct(float v) {
    if (v >= CURVE[0].v) return 100;
    int n = (int)(sizeof(CURVE) / sizeof(CURVE[0]));
    if (v <= CURVE[n - 1].v) return 0;
    for (int i = 1; i < n; i++) {
        if (v >= CURVE[i].v) {   // between CURVE[i-1] (higher) and CURVE[i] (lower)
            float span = CURVE[i - 1].v - CURVE[i].v;
            float frac = span > 0 ? (v - CURVE[i].v) / span : 0;
            return CURVE[i].pct + (int)(frac * (CURVE[i - 1].pct - CURVE[i].pct) + 0.5f);
        }
    }
    return 0;
}

void battery_init(void) {
    adc_oneshot_unit_init_cfg_t init_cfg = {};
    init_cfg.unit_id = BAT_ADC_UNIT;
    if (adc_oneshot_new_unit(&init_cfg, &s_adc) != ESP_OK) { ESP_LOGW(TAG, "adc unit init failed"); return; }
    adc_oneshot_chan_cfg_t chan = {};
    chan.atten = ADC_ATTEN_DB_12;            // full ~0..3.3V range
    chan.bitwidth = ADC_BITWIDTH_12;
    if (adc_oneshot_config_channel(s_adc, BAT_ADC_CHANNEL, &chan) != ESP_OK) { ESP_LOGW(TAG, "adc chan cfg failed"); return; }

    adc_cali_curve_fitting_config_t cali_cfg = {};
    cali_cfg.unit_id = BAT_ADC_UNIT;
    cali_cfg.atten = ADC_ATTEN_DB_12;
    cali_cfg.bitwidth = ADC_BITWIDTH_12;
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "adc calibration unavailable, using raw conversion");
        s_cali = NULL;
    }
    s_ready = true;
}

// Returns false until the ADC is up. Averages a few samples to steady the reading.
bool battery_read(float *volts, int *pct, bool *charging) {
    if (!s_ready || !s_adc) return false;
    const int N = 8;
    long acc_mv = 0; int got = 0;
    for (int i = 0; i < N; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, BAT_ADC_CHANNEL, &raw) != ESP_OK) continue;
        int mv;
        if (s_cali) { if (adc_cali_raw_to_voltage(s_cali, raw, &mv) != ESP_OK) continue; }
        else        { mv = (int)((float)raw * 3300.0f / 4096.0f); }
        acc_mv += mv; got++;
    }
    if (!got) return false;
    float v = (acc_mv / (float)got) * 0.001f * BAT_DIVIDER;
    if (volts)    *volts    = v;
    if (pct)      *pct      = volts_to_pct(v);
    if (charging) *charging = (v >= CHARGING_VOLTS);
    return true;
}
