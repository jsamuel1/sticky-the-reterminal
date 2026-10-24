// tiny_batt_curve — pack voltage -> percent for a board with no fuel gauge.
//
// Pure (no IDF), so tests/host/batt_curve_host_test.sh compiles the shipped
// file. The ADC glue (sticky_battery_adc.cpp) takes N calibrated pin readings
// and hands them here; this module owns every judgement made on them:
//
//   * median, not mean: one ADC spike (Wi-Fi TX, the LCD backlight PWM edge)
//     moves a mean, not a median;
//   * the divider is undone AFTER the median, on the full-precision value;
//   * a reading is only turned into a percent when it is plausible for a
//     single Li-ion/LiPo cell. Out of range, or samples that disagree with
//     each other by more than a resting cell can (a charger hunting with no
//     cell on USB), answer "no percent" - never a guess.
//
// The percent is a resting open-circuit-voltage estimate: under load it reads
// low, on charge it reads high (and clamps at 100). It is the best a divider
// can say, and the field is named for what it is in `sensors` (source:"adc").
#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Single-cell window outside which no percent is reported. Below 3000 mV a
// LiPo is past its protection cut-off (or absent and the pin is floating
// low); above 4350 mV is no cell chemistry this board charges.
#define TINY_BATT_MIN_PLAUSIBLE_MV 3000
#define TINY_BATT_MAX_PLAUSIBLE_MV 4350
// Max spread (max - min, in PACK millivolts) across one burst of samples.
// A cell is a big capacitor: within ~2 ms its voltage does not move 150 mV.
// A charger output with no cell behind it does.
#define TINY_BATT_MAX_SPREAD_MV 150

typedef struct {
    int pack_mv;     // median pin mV x divider; -1 if no samples
    int spread_mv;   // (max - min) pin mV x divider across the burst
    int percent;     // 0..100, or -1 = implausible / unknown (report null)
    bool plausible;  // pack_mv inside the window AND spread small
} tiny_batt_estimate_t;

// Median of n values (n >= 1). Sorts `mv` in place.
int tiny_batt_median_mv(int *mv, size_t n);

// Piecewise-linear LiPo resting-OCV curve. Clamps to 0..100 inside the
// plausible window; -1 outside it.
int tiny_batt_lipo_percent(int pack_mv);

// Everything above in one call. `pin_mv` are calibrated millivolts AT THE
// PIN (sorted in place); `divider` is the ratio pack/pin (2 on FNK0104S).
tiny_batt_estimate_t tiny_batt_estimate(int *pin_mv, size_t n, int divider);

#ifdef __cplusplus
}
#endif
