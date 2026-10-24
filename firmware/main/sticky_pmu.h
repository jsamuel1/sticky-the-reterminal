#pragma once

// sticky_pmu — the AXP2101 power key on the AMOLED-1.8 (HAL_PMU_AXP2101).
//
// The battery and supply readers live behind sticky_battery.h like every
// other board's. This header is only the part no other board has: the PWR
// key, which is not a GPIO. It is an AXP2101 interrupt, and the PMU's IRQ
// line goes to TCA9554 P5, not to the ESP32-S3 (the vendor's own
// 90_axp2101_pmu sdkconfig.defaults sets CONFIG_PMU_INTERRUPT_PIN=-1). So the
// key is read by polling the PMU's latched interrupt status over I2C.

#include "esp_err.h"

enum class PmuKey : unsigned char { None, Short, Long };

// Reads and clears the PWR key bits of INTSTS2. Press events latch in the
// PMU until cleared, so a poll every ~100 ms loses nothing. ESP_OK with
// PmuKey::None means "no press since the last call"; any error means the
// PMU did not answer.
esp_err_t sticky_pmu_take_key(PmuKey &key);
