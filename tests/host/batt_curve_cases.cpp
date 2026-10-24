// batt_curve host test — compiles the SHIPPED firmware/main/tiny_batt_curve.cpp.
#include "tiny/tiny_batt_curve.h"

#include <cstdio>

static int fails = 0;
#define CHECK(name, cond) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); fails++; } \
} while (0)

int main()
{
    // ---- median ----
    {
        int a[] = {5};
        CHECK("median n=1", tiny_batt_median_mv(a, 1) == 5);
        int b[] = {9, 1, 5};
        CHECK("median odd, unsorted", tiny_batt_median_mv(b, 3) == 5);
        int c[] = {4, 1, 3, 2};
        CHECK("median even rounds half up", tiny_batt_median_mv(c, 4) == 3);
        int d[] = {1900, 1901, 1899, 2600, 1900};
        CHECK("median ignores one spike", tiny_batt_median_mv(d, 5) == 1900);
        CHECK("median n=0 is -1", tiny_batt_median_mv(a, 0) == -1);
        CHECK("median null is -1", tiny_batt_median_mv(nullptr, 3) == -1);
    }

    // ---- curve ----
    CHECK("4200 mV = 100%", tiny_batt_lipo_percent(4200) == 100);
    CHECK("4300 mV clamps 100%", tiny_batt_lipo_percent(4300) == 100);
    CHECK("3840 mV = 50%", tiny_batt_lipo_percent(3840) == 50);
    CHECK("3270 mV = 0%", tiny_batt_lipo_percent(3270) == 0);
    CHECK("3100 mV clamps 0%", tiny_batt_lipo_percent(3100) == 0);
    CHECK("3440 mV interpolates (0..5) to 3%",
          tiny_batt_lipo_percent(3440) == 3);  // 170*5/340 = 2.5 -> 3
    CHECK("4000 mV between 75 and 80", tiny_batt_lipo_percent(4000) == 78);
    CHECK("2999 mV implausible", tiny_batt_lipo_percent(2999) == -1);
    CHECK("4351 mV implausible", tiny_batt_lipo_percent(4351) == -1);
    {
        bool mono = true;
        int prev = tiny_batt_lipo_percent(3000);
        for (int mv = 3001; mv <= 4350; ++mv) {
            const int p = tiny_batt_lipo_percent(mv);
            if (p < prev || p < 0 || p > 100) mono = false;
            prev = p;
        }
        CHECK("curve monotone 0..100 across window", mono);
    }

    // ---- estimate: divider applied to the full-precision median ----
    {
        // Pin 1951 mV x2 = 3902. A sum/N*2 integer path would read 3902 too,
        // so use values whose mean truncates: 1951,1952,1951 -> median 1951.
        int s[] = {1952, 1951, 1951};
        tiny_batt_estimate_t e = tiny_batt_estimate(s, 3, 2);
        CHECK("pack = median x divider", e.pack_mv == 3902);
        CHECK("spread x divider", e.spread_mv == 2);
        CHECK("plausible resting cell", e.plausible);
        CHECK("percent from curve", e.percent == tiny_batt_lipo_percent(3902));
    }
    {
        // Odd pin mV: x2 must keep the LSB. 1921 -> 3842, not 3840.
        int s[] = {1921, 1921, 1921, 1921, 1921};
        tiny_batt_estimate_t e = tiny_batt_estimate(s, 5, 2);
        CHECK("odd pin mV keeps 2 mV precision", e.pack_mv == 3842);
    }
    {
        // USB, no cell: charger output hunting 3.3..4.2 V within one burst.
        int s[] = {1650, 2100, 1700, 2050, 1800, 2000, 1900};
        tiny_batt_estimate_t e = tiny_batt_estimate(s, 7, 2);
        CHECK("hunting charger: implausible", !e.plausible);
        CHECK("hunting charger: no percent", e.percent == -1);
        CHECK("hunting charger: voltage still reported", e.pack_mv == 3800);
    }
    {
        // No cell, no charger: pin pulled to ~0.
        int s[] = {12, 10, 11};
        tiny_batt_estimate_t e = tiny_batt_estimate(s, 3, 2);
        CHECK("floating low: implausible", !e.plausible && e.percent == -1);
    }
    {
        // Above the window (wrong divider assumption / not a LiPo).
        int s[] = {2300, 2300, 2300};
        tiny_batt_estimate_t e = tiny_batt_estimate(s, 3, 2);
        CHECK("4600 mV: implausible", !e.plausible && e.percent == -1);
    }
    {
        // Spread exactly at the limit is accepted; one over is not.
        int ok[] = {1900, 1975};
        CHECK("spread 150 accepted", tiny_batt_estimate(ok, 2, 2).plausible);
        int bad[] = {1900, 1976};
        CHECK("spread 152 refused", !tiny_batt_estimate(bad, 2, 2).plausible);
    }
    {
        int s[] = {1};
        CHECK("n=0 -> unknown", tiny_batt_estimate(s, 0, 2).pack_mv == -1);
        CHECK("divider 0 -> unknown", tiny_batt_estimate(s, 1, 0).percent == -1);
    }

    printf(fails ? "batt_curve: %d FAILED\n" : "batt_curve: all passed\n", fails);
    return fails ? 1 : 0;
}
