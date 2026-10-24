// sticky_battery_adc — the battery on a board with a divider and no gauge.
//
// FNK0104S: the cell feeds GPIO 9 (ADC1 channel 8 on the ESP32-S3) through a
// 2:1 resistor divider. This file implements the whole sticky_battery API on
// it: a burst of calibrated readings goes to tiny_batt_curve.h, which takes
// the median, undoes the divider, and estimates a percent from a LiPo
// resting-voltage curve - but only when the burst looks like a resting single
// cell. USB with no cell (pin floating, or a charger hunting) answers
// ESP_ERR_INVALID_RESPONSE from the percent readers, so `status` reports
// battery_pct null rather than a fabricated number; battery_mv still reports
// what was measured. Charging cannot be seen by a divider: charging_known is
// false and `status` reports charging null.
//
// Calibration: curve fitting from the eFuse (every S3 has it). Without a
// calibration scheme a raw count is not millivolts, so the read refuses.
#include "sticky_battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "pin_config.h"
#include "tiny/tiny_batt_curve.h"

#ifdef PIN_BAT_ADC

namespace {
constexpr const char *kTag = "battery_adc";
constexpr int kDivider = 2;     // 2:1 divider: the pin sees half the pack
constexpr int kSamples = 15;    // odd: a true median (~1 ms total)

adc_oneshot_unit_handle_t s_unit = nullptr;
adc_cali_handle_t s_cali = nullptr;
adc_channel_t s_chan = ADC_CHANNEL_0;
bool s_tried = false;
esp_err_t s_init_err = ESP_ERR_INVALID_STATE;

esp_err_t lazy_init() {
    if (s_tried) return s_init_err;
    s_tried = true;
    adc_unit_t unit;
    esp_err_t err = adc_oneshot_io_to_channel(PIN_BAT_ADC, &unit, &s_chan);
    if (err != ESP_OK) return s_init_err = err;
    adc_oneshot_unit_init_cfg_t ucfg = {};
    ucfg.unit_id = unit;
    if ((err = adc_oneshot_new_unit(&ucfg, &s_unit)) != ESP_OK) return s_init_err = err;
    // Any failure after the unit exists releases it: the error is cached
    // (s_tried), so nothing would ever free it later.
    auto fail = [](esp_err_t e) {
        adc_oneshot_del_unit(s_unit);
        s_unit = nullptr;
        return s_init_err = e;
    };
    adc_oneshot_chan_cfg_t ccfg = {};
    ccfg.atten = ADC_ATTEN_DB_12;    // 0..~3.1 V at the pin = 0..~6.2 V pack
    ccfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if ((err = adc_oneshot_config_channel(s_unit, s_chan, &ccfg)) != ESP_OK)
        return fail(err);
    adc_cali_curve_fitting_config_t kcfg = {};
    kcfg.unit_id = unit;
    kcfg.chan = s_chan;
    kcfg.atten = ADC_ATTEN_DB_12;
    kcfg.bitwidth = ADC_BITWIDTH_DEFAULT;
    if ((err = adc_cali_create_scheme_curve_fitting(&kcfg, &s_cali)) != ESP_OK) {
        ESP_LOGW(kTag, "no ADC calibration (%s) - voltage unavailable",
                 esp_err_to_name(err));
        return fail(err);
    }
    ESP_LOGI(kTag, "pack voltage on GPIO %d (ADC%d ch %d), divider %d:1",
             PIN_BAT_ADC, (int)unit + 1, (int)s_chan, kDivider);
    return s_init_err = ESP_OK;
}

// One burst, judged. Callers arrive from the node task (status, glance verb,
// auto-sleep) and the display path (title-bar cluster), and the oneshot
// driver's read is not documented thread-safe, so one mutex covers init and
// the burst. A 1 s cache keeps the readers of one `status` reply consistent
// and makes the per-card cluster read free.
SemaphoreHandle_t lock()
{
    static StaticSemaphore_t buf;
    static SemaphoreHandle_t m = xSemaphoreCreateMutexStatic(&buf);
    return m;
}

constexpr int64_t kCacheUs = 1000000;
int64_t s_at_us = -1;
tiny_batt_estimate_t s_last = {};

esp_err_t estimate(tiny_batt_estimate_t &out)
{
    xSemaphoreTake(lock(), portMAX_DELAY);
    esp_err_t err = lazy_init();
    const int64_t now = esp_timer_get_time();
    if (err == ESP_OK && (s_at_us < 0 || now - s_at_us > kCacheUs)) {
        int pin_mv[kSamples];
        for (int i = 0; i < kSamples && err == ESP_OK; ++i) {
            int raw = 0;
            err = adc_oneshot_read(s_unit, s_chan, &raw);
            if (err == ESP_OK) err = adc_cali_raw_to_voltage(s_cali, raw, &pin_mv[i]);
        }
        if (err == ESP_OK) {
            s_last = tiny_batt_estimate(pin_mv, kSamples, kDivider);
            s_at_us = now;
            ESP_LOGD(kTag, "pack %d mV spread %d mV -> %d%% (%s)", s_last.pack_mv,
                     s_last.spread_mv, s_last.percent,
                     s_last.plausible ? "plausible" : "not a resting cell");
        }
    }
    if (err == ESP_OK) out = s_last;
    xSemaphoreGive(lock());
    return err;
}
}  // namespace

esp_err_t sticky_battery_init(i2c_master_bus_handle_t)
{
    // No I2C part: the ADC is the gauge. Bring it up now so boot logs say
    // whether calibration exists, instead of at the first read.
    tiny_batt_estimate_t e;
    const esp_err_t err = estimate(e);
    if (err == ESP_OK)
        ESP_LOGI(kTag, "pack %d mV (spread %d mV): %s", e.pack_mv, e.spread_mv,
                 e.plausible ? "cell present" : "no plausible cell - percent null");
    return err;
}

esp_err_t sticky_battery_read_voltage(int &millivolts)
{
    tiny_batt_estimate_t e;
    const esp_err_t err = estimate(e);
    if (err != ESP_OK) return err;
    millivolts = e.pack_mv;  // the measurement, plausible or not
    return ESP_OK;
}

esp_err_t sticky_battery_read_detail(BatteryDetail &detail)
{
    detail.source = "adc";  // so a failure is attributed to the right part
    tiny_batt_estimate_t e;
    const esp_err_t err = estimate(e);
    if (err != ESP_OK) return err;
    // USB with no cell, a dead cell, or a charger hunting: not a percent.
    if (!e.plausible) return ESP_ERR_INVALID_RESPONSE;
    detail = BatteryDetail{};
    detail.percent = e.percent;
    detail.voltage_mv = e.pack_mv;
    detail.present = true;
    detail.source = "adc";
    detail.charging_known = false;  // a divider sees no current
    return ESP_OK;
}

esp_err_t sticky_battery_read(BatteryReading &reading)
{
    BatteryDetail d;
    const esp_err_t err = sticky_battery_read_detail(d);
    if (err == ESP_OK) reading.percent = d.percent;
    return err;
}

#endif  // PIN_BAT_ADC
