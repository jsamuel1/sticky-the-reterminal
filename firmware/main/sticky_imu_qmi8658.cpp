// sticky_imu (AMOLED-1.8 V2) — QMI8658 accelerometer, one-shot reads.
//
// Same API as the reTerminal's LSM6DS3TR-C driver (sticky_imu.h), so
// tiny_orient, tiny_lock and the `sensors` verb need no board #if.
//
// Accelerometer only, at a low rate: +/-2 g, 31.25 Hz, gyro never enabled.
// The orient task samples a few times a second; nothing here needs more, and
// an idle gyro is current for nothing. sticky_imu_read_gyro() therefore
// answers ESP_ERR_NOT_SUPPORTED (the `sensors` reply says so in errors[]).
//
// Interrupts: the schematic routes QMI_INT1 to TCA9554 EXIO6 and leaves INT2
// unconnected, so data-ready would cost an expander read anyway. v1 polls.
#include "sticky_imu.h"

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "pin_config.h"

namespace {

constexpr char kTag[] = "sticky_imu";
constexpr uint32_t kI2cClockHz = 400000;
constexpr int kI2cTimeoutMs = 100;

// QMI8658 register map (datasheet rev 0.9).
constexpr uint8_t kRegWhoAmI = 0x00;
constexpr uint8_t kWhoAmI = 0x05;
constexpr uint8_t kRegCtrl1 = 0x02;   // serial interface
constexpr uint8_t kRegCtrl2 = 0x03;   // accelerometer full scale + ODR
constexpr uint8_t kRegCtrl7 = 0x08;   // sensor enables
constexpr uint8_t kRegAxL = 0x35;     // AX_L..AZ_H, 6 bytes
constexpr uint8_t kRegReset = 0x60;
constexpr uint8_t kResetCmd = 0xB0;

constexpr uint8_t kCtrl1AutoIncLe = 0x40;  // ADDR_AI, little-endian, osc on
constexpr uint8_t kCtrl2Acc2g31Hz = 0x08;  // aFS=+/-2g (000), aODR=31.25 Hz
constexpr uint8_t kCtrl7AccOnly = 0x01;    // aEN; gEN=0
constexpr float kScaleG = 1.0F / 16384.0F; // +/-2 g
constexpr float kOrientationThresholdG = 0.70F;

i2c_master_dev_handle_t s_device = nullptr;

esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t data[] = {reg, value};
    return i2c_master_transmit(s_device, data, sizeof(data), kI2cTimeoutMs);
}

esp_err_t read_registers(uint8_t reg, uint8_t *data, size_t length)
{
    return i2c_master_transmit_receive(s_device, &reg, 1, data, length,
                                       kI2cTimeoutMs);
}

int16_t read_int16_le(const uint8_t *data)
{
    return static_cast<int16_t>(
        (static_cast<uint16_t>(data[1]) << 8) | data[0]);
}

// Coarse pose for the `sensors` verb. Axis meaning on this board is in
// tiny/tiny_orient_map.h (kTinyOrientMapAmoled18); the names follow it:
// up along +X = native top edge up, and the chip faces away from the glass,
// so glass-up reads z < 0.
StickyImuOrientation classify_orientation(float x, float y, float z)
{
    const float abs_x = std::fabs(x);
    const float abs_y = std::fabs(y);
    const float abs_z = std::fabs(z);
    if (abs_z >= kOrientationThresholdG && abs_z > abs_x && abs_z > abs_y)
        return z < 0.0F ? StickyImuOrientation::FaceUp
                        : StickyImuOrientation::FaceDown;
    if (abs_x >= kOrientationThresholdG && abs_x > abs_y)
        return x > 0.0F ? StickyImuOrientation::ArrowUp
                        : StickyImuOrientation::ArrowDown;
    if (abs_y >= kOrientationThresholdG)
        return y < 0.0F ? StickyImuOrientation::ArrowLeft
                        : StickyImuOrientation::ArrowRight;
    return StickyImuOrientation::Unknown;
}

esp_err_t fail(esp_err_t err, const char *what)
{
    ESP_LOGE(kTag, "qmi8658 %s: %s", what, esp_err_to_name(err));
    i2c_master_bus_rm_device(s_device);
    s_device = nullptr;
    return err;
}

}  // namespace

esp_err_t sticky_imu_init(i2c_master_bus_handle_t bus)
{
    if (bus == nullptr) return ESP_ERR_INVALID_ARG;
    if (s_device != nullptr) return ESP_OK;

    i2c_device_config_t config = {};
    config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    config.device_address = QMI8658_I2C_ADDR;
    config.scl_speed_hz = kI2cClockHz;
    esp_err_t err = i2c_master_bus_add_device(bus, &config, &s_device);
    if (err != ESP_OK) return err;

    uint8_t who = 0;
    err = read_registers(kRegWhoAmI, &who, 1);
    if (err != ESP_OK) return fail(err, "WHO_AM_I read");
    if (who != kWhoAmI) {
        ESP_LOGE(kTag, "qmi8658 WHO_AM_I 0x%02X, want 0x%02X", who, kWhoAmI);
        return fail(ESP_ERR_NOT_FOUND, "probe");
    }

    // Soft reset: a warm reboot must not inherit a gyro or ODR some earlier
    // firmware left running.
    err = write_register(kRegReset, kResetCmd);
    if (err != ESP_OK) return fail(err, "reset");
    vTaskDelay(pdMS_TO_TICKS(20));

    err = write_register(kRegCtrl1, kCtrl1AutoIncLe);
    if (err == ESP_OK) err = write_register(kRegCtrl2, kCtrl2Acc2g31Hz);
    if (err == ESP_OK) err = write_register(kRegCtrl7, kCtrl7AccOnly);
    if (err != ESP_OK) return fail(err, "configure");

    // A few 31.25 Hz periods before the output registers hold a real sample.
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_LOGI(kTag, "QMI8658 at 0x%02X: accelerometer +/-2g 31.25Hz, gyro off",
             QMI8658_I2C_ADDR);
    return ESP_OK;
}

esp_err_t sticky_imu_read(StickyImuState &state)
{
    if (s_device == nullptr) return ESP_ERR_INVALID_STATE;
    uint8_t data[6] = {};
    const esp_err_t err = read_registers(kRegAxL, data, sizeof(data));
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "accelerometer read failed: %s", esp_err_to_name(err));
        state.valid = false;
        return err;
    }
    state.acceleration_x_g = read_int16_le(data) * kScaleG;
    state.acceleration_y_g = read_int16_le(data + 2) * kScaleG;
    state.acceleration_z_g = read_int16_le(data + 4) * kScaleG;
    state.orientation = classify_orientation(state.acceleration_x_g,
                                             state.acceleration_y_g,
                                             state.acceleration_z_g);
    state.valid = true;
    return ESP_OK;
}

esp_err_t sticky_imu_read_gyro(StickyGyroReading &reading)
{
    reading.valid = false;
    return ESP_ERR_NOT_SUPPORTED;  // accelerometer-only driver (see header)
}

const char *sticky_imu_orientation_name(StickyImuOrientation orientation)
{
    switch (orientation) {
        case StickyImuOrientation::ArrowUp:    return "arrow_up";
        case StickyImuOrientation::ArrowDown:  return "arrow_down";
        case StickyImuOrientation::ArrowLeft:  return "arrow_left";
        case StickyImuOrientation::ArrowRight: return "arrow_right";
        case StickyImuOrientation::FaceUp:     return "face_up";
        case StickyImuOrientation::FaceDown:   return "face_down";
        case StickyImuOrientation::Unknown:
        default:                               return "unknown";
    }
}
