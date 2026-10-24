#pragma once

// lcd_band — row-window alignment for colour panels whose controller takes
// windows in units larger than one row (CO5300: 2). Pure, no ESP-IDF:
// tests/host/lcd_band_host_test.sh compiles this header as shipped.

namespace lcd_band {

// Widen [*y0, *y1) outward to multiples of `align`. `height` must itself be a
// multiple of `align` (lcd_canvas refuses a panel that is not), so the widened
// band never leaves [0, height). align <= 1 leaves the band untouched.
inline void align(int align, int *y0, int *y1) {
    if (align <= 1) return;
    *y0 -= *y0 % align;
    *y1 += (align - *y1 % align) % align;
}

}  // namespace lcd_band
