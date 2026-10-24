// sticky_imu — board-selected accelerometer, one-shot reads.
//
// The reTerminal implementation drives its LSM6DS3TR-C; AMOLED-1.8 drives its
// QMI8658. Both expose current acceleration through this shared API, so the
// `sensors` verb and orientation watcher need no board branches. No driver owns
// a monitoring task: sticky_imu_read() performs the I2C burst itself.
//
// Gyroscope support is board-specific. The reTerminal powers its gyro only for
// a read and switches it off again; the accelerometer-only AMOLED driver
// returns ESP_ERR_NOT_SUPPORTED without enabling the gyro.
#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

enum class StickyImuOrientation {
    Unknown,
    ArrowUp,
    ArrowDown,
    ArrowLeft,
    ArrowRight,
    FaceUp,
    FaceDown,
};

struct StickyImuState {
    float acceleration_x_g = 0.0F;
    float acceleration_y_g = 0.0F;
    float acceleration_z_g = 0.0F;
    StickyImuOrientation orientation = StickyImuOrientation::Unknown;
    bool valid = false;
};

struct StickyGyroReading {
    float x_dps = 0.0F;
    float y_dps = 0.0F;
    float z_dps = 0.0F;
    bool valid = false;
};

// Initializes the board accelerometer on the shared sensor I2C bus at its
// board-selected low-power ODR and +/-2 g range.
esp_err_t sticky_imu_init(i2c_master_bus_handle_t bus);

// Reads acceleration + classifies orientation right now. ESP_ERR_INVALID_STATE
// if init never succeeded.
esp_err_t sticky_imu_read(StickyImuState &state);

// Reads one gyro sample when supported. The reTerminal powers the gyro up,
// waits ~110 ms, reads it, then powers it down; accelerometer-only boards return
// ESP_ERR_NOT_SUPPORTED and leave reading.valid false.
esp_err_t sticky_imu_read_gyro(StickyGyroReading &reading);

// Stable lower-case name for JSON/report use ("face_up", "arrow_left", ...).
const char *sticky_imu_orientation_name(StickyImuOrientation orientation);
