// hal_absent — the drivers a board does not have (yet).
//
// Built instead of sticky_battery / sticky_rtc / sticky_sht40 / sticky_imu /
// sticky_sdcard on the colour boards. Every reader gets
// ESP_ERR_NOT_SUPPORTED, so the existing "null over guess" paths in
// tiny_sensors, `status` and the glance strip report "no reading" instead of
// a fabricated value, and nothing in the shared code needs a board #if.
//
// The buzzer API is NOT here: both colour boards drive it through the ES8311
// (sticky_buzzer_es8311.cpp).
#include "sticky_battery.h"
#include "sticky_imu.h"
#include "sticky_rtc.h"
#include "sticky_sdcard.h"
#include "sticky_sht40.h"

// ---- battery gauge ----------------------------------------------------------
esp_err_t sticky_battery_init(i2c_master_bus_handle_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_battery_read(BatteryReading &) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_battery_read_detail(BatteryDetail &) { return ESP_ERR_NOT_SUPPORTED; }

// ---- RTC -------------------------------------------------------------------
esp_err_t sticky_rtc_init(i2c_master_bus_handle_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_rtc_read(RtcDateTime &) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_rtc_write(const RtcDateTime &) { return ESP_ERR_NOT_SUPPORTED; }

// ---- SHT40 -----------------------------------------------------------------
esp_err_t sticky_sht40_init(i2c_master_bus_handle_t) { return ESP_ERR_NOT_SUPPORTED; }
esp_err_t sticky_sht40_read(Sht40Reading &) { return ESP_ERR_NOT_SUPPORTED; }

// ---- IMU -------------------------------------------------------------------
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

// ---- microSD (phase 1: not driven) -----------------------------------------
extern "C" esp_err_t sticky_sdcard_init(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" bool sticky_sdcard_present(void) { return false; }
extern "C" bool sticky_sdcard_mounted(void) { return false; }
extern "C" esp_err_t sticky_sdcard_ensure(void) { return ESP_ERR_NOT_FOUND; }
extern "C" esp_err_t sticky_sdcard_mount(void) { return ESP_ERR_NOT_SUPPORTED; }
extern "C" esp_err_t sticky_sdcard_unmount(void) { return ESP_OK; }
extern "C" esp_err_t sticky_sdcard_info(uint64_t *, uint64_t *) { return ESP_ERR_INVALID_STATE; }
extern "C" void sticky_sdcard_prepare_sleep(void) {}
extern "C" esp_err_t sticky_sdcard_format(void) { return ESP_ERR_NOT_SUPPORTED; }
