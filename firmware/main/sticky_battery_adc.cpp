// sticky_battery_adc — pack voltage on a board with a divider and no gauge.
//
// FNK0104S: the cell feeds GPIO 9 (ADC1 channel 8 on the ESP32-S3) through a
// 2:1 resistor divider. The ADC answers a VOLTAGE; it cannot answer a state
// of charge (a Li-ion's curve is flat, load- and temperature-dependent, and a
// percentage invented from it would be the "fabricated battery" the status
// contract forbids). So this file implements only sticky_battery_read_voltage;
// the percent readers stay NOT_SUPPORTED in hal_absent.cpp and `status` keeps
// battery_pct null while reporting battery_mv.
//
// Calibration: curve fitting from the eFuse (every S3 has it). Without a
// calibration scheme a raw count is not millivolts, so the read refuses.
#include "sticky_battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "pin_config.h"

#ifdef PIN_BAT_ADC

namespace {
constexpr const char *kTag = "battery_adc";
constexpr int kDivider = 2;     // 2:1 divider: the pin sees half the pack
constexpr int kSamples = 16;    // average out ADC noise (~1 ms total)

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
}  // namespace

esp_err_t sticky_battery_read_voltage(int &millivolts)
{
    const esp_err_t ierr = lazy_init();
    if (ierr != ESP_OK) return ierr;
    int sum = 0;
    for (int i = 0; i < kSamples; ++i) {
        int mv = 0;
        int raw = 0;
        esp_err_t err = adc_oneshot_read(s_unit, s_chan, &raw);
        if (err == ESP_OK) err = adc_cali_raw_to_voltage(s_cali, raw, &mv);
        if (err != ESP_OK) return err;
        sum += mv;
    }
    millivolts = sum * kDivider / kSamples;   // multiply first: keep the 2 mV steps
    return ESP_OK;
}

#endif  // PIN_BAT_ADC
