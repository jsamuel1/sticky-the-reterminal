#pragma once

// lcd_gray4 — the pure half of the colour-LCD path: 2-bit canvas bytes in,
// big-endian RGB565 strips out, plus the dirty-row diff the partial refresh
// uses. No ESP-IDF, no panel: tests/host/lcd_gray4_host_test.sh compiles this
// header against the real Canvas, so the bytes a panel receives are the bytes
// the test checked.
//
// Canvas packing (canvas.cpp draw_pixel): 4 pixels per byte, leftmost pixel in
// bits 7..6, level 0 = Black .. 3 = White.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lcd_gray4 {

// The four canvas levels as 8-bit gray: the same 00/55/AA/FF the screenshot
// PNG and tiny_display_snapshot_gray8 use, so glass and screenshot agree.
constexpr uint8_t kLevel8[4] = {0x00, 0x55, 0xAA, 0xFF};

// Gray8 -> native RGB565 (R and B equal, so RGB/BGR panel order is moot).
constexpr uint16_t rgb565(uint8_t v) {
    return static_cast<uint16_t>(((v >> 3) << 11) | ((v >> 2) << 5) | (v >> 3));
}

// SPI panels take the high byte first; esp_lcd sends memory order, so the
// strip holds each pixel byte-swapped.
constexpr uint16_t be16(uint16_t c) {
    return static_cast<uint16_t>((c >> 8) | (c << 8));
}

// One canvas byte -> its four big-endian pixels. 256 x 8 B = 2 KB, built once,
// so the per-strip conversion is a table copy per byte.
struct Lut {
    uint16_t quad[256][4];
};

inline void build(Lut &lut) {
    for (int b = 0; b < 256; ++b)
        for (int i = 0; i < 4; ++i)
            lut.quad[b][i] = be16(rgb565(kLevel8[(b >> (6 - 2 * i)) & 0x3]));
}

// Expand `rows` canvas rows starting at `src` into `dst` (width*rows pixels).
// `width` must be a multiple of 4 (both colour panels are: 320, 368).
inline void expand_rows(const uint8_t *src, size_t stride, int width, int rows,
                        const Lut &lut, uint16_t *dst) {
    const int bytes = width / 4;
    for (int r = 0; r < rows; ++r) {
        const uint8_t *s = src + static_cast<size_t>(r) * stride;
        for (int x = 0; x < bytes; ++x, dst += 4)
            std::memcpy(dst, lut.quad[s[x]], sizeof lut.quad[0]);
    }
}

// Smallest row band [y0, y1) where `a` and `b` differ. False = identical.
inline bool dirty_band(const uint8_t *a, const uint8_t *b, size_t stride,
                       int height, int *y0, int *y1) {
    int top = 0;
    while (top < height &&
           std::memcmp(a + static_cast<size_t>(top) * stride,
                       b + static_cast<size_t>(top) * stride, stride) == 0)
        ++top;
    if (top == height) return false;
    int bot = height;
    while (bot > top &&
           std::memcmp(a + static_cast<size_t>(bot - 1) * stride,
                       b + static_cast<size_t>(bot - 1) * stride, stride) == 0)
        --bot;
    *y0 = top;
    *y1 = bot;
    return true;
}

}  // namespace lcd_gray4
