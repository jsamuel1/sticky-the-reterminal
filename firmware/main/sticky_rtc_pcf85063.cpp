// sticky_rtc_pcf85063 — the AMOLED-1.8 wall clock (PCF85063A at 0x51).
//
// Same API as the reTerminal's PCF8563 driver (sticky_rtc.h), so tiny_time
// and tiny_sensors need nothing new: boot reads UTC from here into the
// system clock, SNTP writes UTC back. The register layout differs from the
// PCF8563 (time starts at 0x04, no century bit, OS flag instead of VL), and
// the decode/encode is tiny/tiny_pmu_util.h, host-tested.
//
// A never-set or battery-lost chip reports its oscillator-stop flag; read()
// answers ESP_ERR_INVALID_STATE then, which tiny_time already treats as
// "clock unset until SNTP". There is no build-timestamp seeding: a clock
// that is wrong by the age of the image would be a fabricated reading.
#include "sticky_rtc.h"

#include "esp_log.h"
#include "pin_config.h"
#include "tiny/tiny_pmu_util.h"

#ifdef HAL_RTC_PCF85063

namespace {

constexpr char kTag[] = "pcf85063";
constexpr uint32_t kI2cClockHz = 400000;
constexpr int kTimeoutMs = 100;

i2c_master_dev_handle_t s_dev = nullptr;

esp_err_t rd(uint8_t reg, uint8_t *buf, size_t n)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    return i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, kTimeoutMs);
}

}  // namespace

esp_err_t sticky_rtc_init(i2c_master_bus_handle_t bus)
{
    if (!bus) return ESP_ERR_INVALID_ARG;
    if (s_dev) return ESP_OK;
    i2c_device_config_t dc = {};
    dc.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dc.device_address = PCF85063_I2C_ADDR;
    dc.scl_speed_hz = kI2cClockHz;
    esp_err_t e = i2c_master_bus_add_device(bus, &dc, &s_dev);
    if (e != ESP_OK) return e;
    uint8_t ctrl1 = 0;
    e = rd(pcf85063::kRegCtrl1, &ctrl1, 1);
    if (e != ESP_OK) {
        ESP_LOGE(kTag, "no PCF85063A at 0x%02X: %s", PCF85063_I2C_ADDR,
                 esp_err_to_name(e));
        i2c_master_bus_rm_device(s_dev);
        s_dev = nullptr;
        return e;
    }
    ESP_LOGI(kTag, "Initialized: I2C address=0x%02X ctrl1=0x%02X%s%s",
             PCF85063_I2C_ADDR, ctrl1,
             (ctrl1 & pcf85063::kCtrl1Stop) ? " (STOPPED)" : "",
             (ctrl1 & pcf85063::kCtrl1Mode12h) ? " (12h mode)" : "");
    return ESP_OK;
}

esp_err_t sticky_rtc_read(RtcDateTime &date_time)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    uint8_t ctrl1 = 0;
    uint8_t raw[7] = {};
    esp_err_t e = rd(pcf85063::kRegCtrl1, &ctrl1, 1);
    if (e == ESP_OK) e = rd(pcf85063::kRegSeconds, raw, sizeof raw);
    if (e != ESP_OK) {
        ESP_LOGE(kTag, "Read failed: %s", esp_err_to_name(e));
        return e;
    }
    // Someone else's 12-hour format or a stopped clock: not a time we can
    // vouch for. A write (SNTP) puts both back the way this driver keeps them.
    if (ctrl1 & (pcf85063::kCtrl1Stop | pcf85063::kCtrl1Mode12h)) {
        ESP_LOGW(kTag, "RTC time invalid: ctrl1=0x%02X (stopped or 12h)", ctrl1);
        return ESP_ERR_INVALID_STATE;
    }
    pcf85063::Fields f;
    switch (pcf85063::decode(raw, f)) {
        case pcf85063::Decode::Ok: break;
        case pcf85063::Decode::OscillatorStopped:
            ESP_LOGW(kTag, "RTC time invalid: oscillator-stop flag is set");
            return ESP_ERR_INVALID_STATE;
        case pcf85063::Decode::BadBcd:
            ESP_LOGW(kTag, "RTC time invalid: malformed BCD data");
            return ESP_ERR_INVALID_RESPONSE;
        case pcf85063::Decode::OutOfRange:
            ESP_LOGW(kTag, "RTC time invalid: value is out of range");
            return ESP_ERR_INVALID_RESPONSE;
    }
    date_time.year = f.year;
    date_time.month = f.month;
    date_time.day = f.day;
    date_time.hour = f.hour;
    date_time.minute = f.minute;
    date_time.second = f.second;
    ESP_LOGI(kTag, "Read success: %04d-%02d-%02d %02d:%02d:%02d", f.year, f.month,
             f.day, f.hour, f.minute, f.second);
    return ESP_OK;
}

esp_err_t sticky_rtc_write(const RtcDateTime &date_time)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    pcf85063::Fields f;
    f.year = date_time.year;
    f.month = date_time.month;
    f.day = date_time.day;
    f.hour = date_time.hour;
    f.minute = date_time.minute;
    f.second = date_time.second;
    if (!pcf85063::valid(f)) {
        ESP_LOGE(kTag, "Refusing to write an invalid date/time");
        return ESP_ERR_INVALID_ARG;
    }
    // Run in 24-hour mode before the time lands, so the hour byte means what
    // encode() wrote. Other CTRL1 bits (capacitor select, CIE) stay as found.
    uint8_t ctrl1 = 0;
    esp_err_t e = rd(pcf85063::kRegCtrl1, &ctrl1, 1);
    if (e != ESP_OK) return e;
    const uint8_t want =
        ctrl1 & static_cast<uint8_t>(~(pcf85063::kCtrl1Stop | pcf85063::kCtrl1Mode12h));
    if (want != ctrl1) {
        const uint8_t c[2] = {pcf85063::kRegCtrl1, want};
        if ((e = i2c_master_transmit(s_dev, c, sizeof c, kTimeoutMs)) != ESP_OK)
            return e;
    }
    uint8_t payload[8] = {pcf85063::kRegSeconds};
    pcf85063::encode(f, payload + 1);  // seconds bit7 = 0 clears the OS flag
    e = i2c_master_transmit(s_dev, payload, sizeof payload, kTimeoutMs);
    if (e != ESP_OK) {
        ESP_LOGE(kTag, "Write failed: %s", esp_err_to_name(e));
        return e;
    }
    ESP_LOGI(kTag, "Set to %04d-%02d-%02d %02d:%02d:%02d (weekday %d)", f.year,
             f.month, f.day, f.hour, f.minute, f.second, payload[5]);
    return ESP_OK;
}

#endif  // HAL_RTC_PCF85063
