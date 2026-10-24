// tiny_sensors — the `sensors` verb: every onboard sensor, read on demand.
//
// SHT40 (ambient temp/RH), LSM6DS3TR-C (acceleration + orientation), PCF8563
// (RTC wall clock), and battery from either the BQ27220 gauge or the FNK0104S
// GPIO 9 ADC divider. AMOLED-1.8: QMI8658, PCF85063A and the AXP2101 PMU.
//
// Two rules this module exists to enforce:
//   1. A source that failed to read emits JSON null plus an `errors` entry
//      naming it. Never a zero or an invented value. Environmental/IMU/RTC
//      reads are fresh; the ADC battery driver deliberately shares one judged
//      burst for 1 s so status and glass cannot disagree.
//   2. No network. `sensors` must answer when the backend is down, exactly
//      like `status` does.
#pragma once
#include <stddef.h>

#include "esp_err.h"
#ifdef __cplusplus
extern "C" {
#endif

// Attaches SHT40 + IMU + RTC to the shared sensor I2C bus. The battery source
// is initialized separately (sticky_battery_init): BQ27220 on reTerminal, ADC
// divider on FNK0104S, AXP2101 on AMOLED-1.8. Returns ESP_OK if at least one I2C sensor came up;
// per-sensor failures are logged and then reported honestly as nulls by
// tiny_sensors_json().
esp_err_t tiny_sensors_init(void);

// Reads everything now and writes a JSON object into `out`:
// {"temperature_c","humidity_pct","orientation","accel_g":{x,y,z},
//  "rtc","battery":{...},"errors":[...],"summary"}
esp_err_t tiny_sensors_json(char *out, size_t cap);

// Paints the same readings as a kv card (card_id "sensors"). No network.
esp_err_t tiny_sensors_render_card(void);

#ifdef __cplusplus
}
#endif
