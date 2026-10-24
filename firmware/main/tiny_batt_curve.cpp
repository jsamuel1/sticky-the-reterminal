#include "tiny/tiny_batt_curve.h"

#include <algorithm>

namespace {

// Resting open-circuit voltage of a 1S LiPo vs state of charge. The widely
// used 5 %-step table (Adafruit/Seeed/LVGL demos agree to ~10 mV); the board
// has no cell-specific data, so a generic curve is the honest choice.
struct Point {
    int mv;
    int pct;
};
constexpr Point kCurve[] = {
    {3270, 0},  {3610, 5},  {3690, 10}, {3710, 15}, {3730, 20}, {3750, 25},
    {3770, 30}, {3790, 35}, {3800, 40}, {3820, 45}, {3840, 50}, {3850, 55},
    {3870, 60}, {3910, 65}, {3950, 70}, {3980, 75}, {4020, 80}, {4080, 85},
    {4110, 90}, {4150, 95}, {4200, 100},
};
constexpr size_t kCurveN = sizeof kCurve / sizeof kCurve[0];

static_assert(kCurve[0].pct == 0 && kCurve[kCurveN - 1].pct == 100,
              "curve must span 0..100");

}  // namespace

extern "C" int tiny_batt_median_mv(int *mv, size_t n)
{
    if (mv == nullptr || n == 0) return -1;
    std::sort(mv, mv + n);
    // Even n: midpoint of the middle two, rounded half up - deterministic,
    // and computed without a+b so no int input can overflow.
    if (n % 2) return mv[n / 2];
    return mv[n / 2 - 1] + (mv[n / 2] - mv[n / 2 - 1] + 1) / 2;
}

extern "C" int tiny_batt_lipo_percent(int pack_mv)
{
    if (pack_mv < TINY_BATT_MIN_PLAUSIBLE_MV ||
        pack_mv > TINY_BATT_MAX_PLAUSIBLE_MV)
        return -1;
    if (pack_mv <= kCurve[0].mv) return 0;
    if (pack_mv >= kCurve[kCurveN - 1].mv) return 100;
    for (size_t i = 1; i < kCurveN; ++i) {
        if (pack_mv <= kCurve[i].mv) {
            const Point a = kCurve[i - 1], b = kCurve[i];
            // Rounded linear interpolation between the two bracketing points.
            const int num = (pack_mv - a.mv) * (b.pct - a.pct);
            const int den = b.mv - a.mv;
            return a.pct + (num + den / 2) / den;
        }
    }
    return 100;  // unreachable: last point handled above
}

extern "C" tiny_batt_estimate_t tiny_batt_estimate(int *pin_mv, size_t n,
                                                  int divider)
{
    tiny_batt_estimate_t e = {-1, 0, -1, false};
    if (pin_mv == nullptr || n == 0 || divider <= 0) return e;
    const int med = tiny_batt_median_mv(pin_mv, n);  // sorts ascending
    // Divider undone on the median itself - no integer division in between.
    e.pack_mv = med * divider;
    // Spread = interquartile range, so ONE spike (which the median already
    // ignores) cannot veto the percent either; a charger hunting with no cell
    // moves the whole burst and still trips it. Below 4 samples there are no
    // quartiles to speak of: max - min.
    const size_t lo = n >= 4 ? n / 4 : 0;
    const size_t hi = n >= 4 ? (3 * n) / 4 : n - 1;
    e.spread_mv = (pin_mv[hi] - pin_mv[lo]) * divider;
    e.plausible = e.pack_mv >= TINY_BATT_MIN_PLAUSIBLE_MV &&
                  e.pack_mv <= TINY_BATT_MAX_PLAUSIBLE_MV &&
                  e.spread_mv <= TINY_BATT_MAX_SPREAD_MV;
    e.percent = e.plausible ? tiny_batt_lipo_percent(e.pack_mv) : -1;
    return e;
}
