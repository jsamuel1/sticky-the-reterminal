// hal_absent — the drivers a board does not have (yet).
//
// Built instead of sticky_battery / sticky_rtc / sticky_sht40 / sticky_imu /
// sticky_sdcard on the colour boards. Every reader gets
// ESP_ERR_NOT_SUPPORTED, so the existing "null over guess" paths in
// tiny_sensors, `status` and the glance strip report "no reading" instead of
// a fabricated value, and nothing in the shared code needs a board #if.
// A board that drives one of these parts defines its HAL_* macro in
// pin_config_<board>.h and builds the real driver instead.
//
// The buzzer API is NOT here: both colour boards drive it through the ES8311
// (sticky_buzzer_es8311.cpp).
#include "pin_config.h"
#include "sticky_battery.h"
#include "sticky_imu.h"
#include "sticky_rtc.h"
#include "sticky_sdcard.h"
#include "sticky_sht40.h"

// ---- battery gauge ----------------------------------------------------------
#if !defined(HAL_PMU_AXP2101)
// The AMOLED-1.8 builds sticky_pmu_axp2101.cpp for the whole API instead.
#ifndef PIN_BAT_ADC
// Boards with a pack-voltage divider build sticky_battery_adc.cpp instead.
esp_err_t sticky_battery_init(i2c_master_bus_handle_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_battery_read(BatteryReading &) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_battery_read_detail(BatteryDetail &) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_battery_read_voltage(int &) { return ESP_ERR_NOT_SUPPORTED; }
#endif
// Neither "nothing" nor a divider can see the USB input.
esp_err_t sticky_battery_read_supply(SupplyReading &) { return ESP_ERR_NOT_SUPPORTED; }
#endif  // HAL_PMU_AXP2101

// ---- RTC -------------------------------------------------------------------
#ifndef HAL_RTC_PCF85063
// The AMOLED-1.8 builds sticky_rtc_pcf85063.cpp instead.
esp_err_t sticky_rtc_init(i2c_master_bus_handle_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_rtc_read(RtcDateTime &) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_rtc_write(const RtcDateTime &) { return ESP_ERR_NOT_SUPPORTED; }
#endif

// ---- SHT40 -----------------------------------------------------------------
esp_err_t sticky_sht40_init(i2c_master_bus_handle_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_sht40_read(Sht40Reading &) { return ESP_ERR_NOT_SUPPORTED; }

// ---- IMU -------------------------------------------------------------------
#ifndef QMI8658_I2C_ADDR
// The AMOLED-1.8 builds sticky_imu_qmi8658.cpp instead.
esp_err_t sticky_imu_init(i2c_master_bus_handle_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_imu_read(StickyImuState &) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_imu_read_gyro(StickyGyroReading &) { return ESP_ERR_NOT_SUPPORTED; }

const char *sticky_imu_orientation_name(StickyImuOrientation orientation)
{
    switch (orientation) {
        case StickyImuOrientation::ArrowUp: return "arrow_up";
        case StickyImuOrientation::ArrowDown: return "arrow_down";
        case StickyImuOrientation::ArrowLeft: return "arrow_left";
        case StickyImuOrientation::ArrowRight: return "arrow_right";
        case StickyImuOrientation::FaceUp: return "face_up";
        case StickyImuOrientation::FaceDown: return "face_down";
        case StickyImuOrientation::Unknown: break;
    }
    return "unknown";
}
#endif  // QMI8658_I2C_ADDR

// ---- microSD ---------------------------------------------------------------
// Boards that build sticky_sdcard_sdmmc.cpp define SD_SDMMC_DRIVEN in their
// pin_config (pins alone are not enough: AMOLED-1.8 records its SD pins but
// does not drive the slot yet).
#ifndef SD_SDMMC_DRIVEN
extern "C" esp_err_t sticky_sdcard_init(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool sticky_sdcard_present(void) { return false; }
extern "C" bool sticky_sdcard_mounted(void) { return false; }
extern "C" esp_err_t sticky_sdcard_ensure(void) { return ESP_ERR_NOT_FOUND; }
extern "C" esp_err_t sticky_sdcard_mount(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t sticky_sdcard_unmount(void) { return ESP_OK; }
extern "C" esp_err_t sticky_sdcard_info(uint64_t *, uint64_t *) { return ESP_ERR_INVALID_STATE; }
extern "C" void sticky_sdcard_report(sticky_sdcard_report_t *out) {
    if (out) *out = {false, false, STICKY_SD_STATE_NO_CARD, 0, 0};
}
extern "C" void sticky_sdcard_prepare_sleep(void) {}
extern "C" esp_err_t sticky_sdcard_format(void) { return ESP_ERR_NOT_SUPPORTED; }
#endif
