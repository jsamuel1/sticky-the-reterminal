#pragma once

#include "esp_err.h"
#include "driver/i2c_master.h"

// Keeps the board powered and puts shared peripherals into a safe idle state.
esp_err_t board_init();

// Shared I2C1 bus used by the onboard sensors.
i2c_master_bus_handle_t board_sensor_i2c_bus();

// AMOLED-1.8 only (board_generic.cpp): pulse the CST820 reset, TCA9554 bit 2,
// low then high, leaving every other expander output as it was. Returns once
// the controller has had time to come out of reset.
esp_err_t board_touch_reset();
