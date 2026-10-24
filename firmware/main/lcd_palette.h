#pragma once

// lcd_palette — the pure half of the colour-panel path: the 2-bit gray canvas
// plane plus the optional 4-bit accent plane in, big-endian RGB565 strips out,
// plus the dirty-row diff the partial refresh uses. No ESP-IDF, no panel:
// tests/host/lcd_palette_host_test.sh compiles this header against the real
// Canvas, so the bytes a panel receives are the bytes the test checked.
//
// Gray plane packing (canvas.cpp draw_pixel): 4 pixels per byte, leftmost
// pixel in bits 7..6, level 0 = Black .. 3 = White.
// Accent plane packing: 2 pixels per byte, leftmost pixel in the high nibble.
//
// A pixel's colour is palette[accent][gray], resolved per theme. The gray
// level keeps its meaning under every accent, which is why one renderer can
// serve the e-ink and the colour glass:
//   Black     -> the accent's ink (text, rules, filled buttons)
//   DarkGray  -> ink at 2/3 strength (secondary text)
//   LightGray -> a 1/4 tint (bubbles, key faces, scroll track)
//   White     -> the theme background
// The e-ink never attaches an accent plane, so none of this reaches it.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace lcd_palette {

// The four canvas levels as 8-bit gray: the same 00/55/AA/FF the gray
// screenshot PNG uses, so glass and screenshot agree.
constexpr uint8_t kLevel8[4] = {0x00, 0x55, 0xAA, 0xFF};

enum class Theme : uint8_t { Light = 0, Dark = 1 };

// Accent indices. The card grammar names them (accent_from_name); 10..15 are
// unassigned and draw as neutral, so a newer sender degrades to gray, never
// to garbage.
enum : uint8_t {
    kNeutral = 0,  // theme gray: black-on-white (light) / light-on-black (dark)
    kMono = 1,     // literal gray in every theme: raw frames, QR modules
    kRed = 2,
    kOrange = 3,
    kYellow = 4,
    kGreen = 5,
    kTeal = 6,
    kBlue = 7,
    kPurple = 8,
    kPink = 9,
    kAccentCount = 16,
};
constexpr int kNamedAccents = 10;  // 0..9 have a meaning

struct Rgb {
    uint8_t r, g, b;
};

// Ink per accent. Light-theme inks are dark enough for text on white;
// dark-theme inks bright enough for text on black. Index 0/1 unused here.
constexpr Rgb kInkLight[kNamedAccents] = {
    {0, 0, 0},       {0, 0, 0},       {0xC6, 0x28, 0x28}, {0xB9, 0x4A, 0x00},
    {0x9A, 0x7B, 0x00}, {0x2E, 0x7D, 0x32}, {0x00, 0x79, 0x6B}, {0x15, 0x65, 0xC0},
    {0x6A, 0x1B, 0x9A}, {0xC2, 0x18, 0x5B},
};
constexpr Rgb kInkDark[kNamedAccents] = {
    {0, 0, 0},       {0, 0, 0},       {0xFF, 0x6B, 0x6B}, {0xFF, 0xA9, 0x4D},
    {0xFF, 0xD4, 0x3B}, {0x69, 0xDB, 0x7C}, {0x4F, 0xD1, 0xC5}, {0x74, 0xC0, 0xFC},
    {0xB1, 0x97, 0xFC}, {0xF7, 0x83, 0xAC},
};
// Dark-theme neutral: the light grays inverted, ink kept off full white
// (an AMOLED should not hold large static #FFFFFF areas).
constexpr uint8_t kDarkNeutral8[4] = {0xE8, 0xA0, 0x3A, 0x00};

constexpr Rgb gray(uint8_t v) { return Rgb{v, v, v}; }

// p/q of `ink` over `bg`, per channel, rounded.
constexpr uint8_t mixc(uint8_t ink, uint8_t bg, int p, int q) {
    return static_cast<uint8_t>((ink * p + bg * (q - p) + q / 2) / q);
}
constexpr Rgb mix(Rgb ink, Rgb bg, int p, int q) {
    return Rgb{mixc(ink.r, bg.r, p, q), mixc(ink.g, bg.g, p, q),
               mixc(ink.b, bg.b, p, q)};
}

// The one colour definition: glass (via rgb565) and screenshot PLTE both
// read it.
constexpr Rgb color(Theme t, uint8_t accent, uint8_t level) {
    level &= 3;
    accent &= 0x0F;
    if (accent == kMono) return gray(kLevel8[level]);
    if (accent == kNeutral || accent >= kNamedAccents)
        return t == Theme::Light ? gray(kLevel8[level]) : gray(kDarkNeutral8[level]);
    const Rgb bg = t == Theme::Light ? gray(0xFF) : gray(0x00);
    const Rgb ink = t == Theme::Light ? kInkLight[accent] : kInkDark[accent];
    switch (level) {
        case 0: return ink;
        case 1: return mix(ink, bg, 2, 3);
        case 2: return mix(ink, bg, 1, 4);
        default: return bg;
    }
}

// Palette index of one pixel: what the screenshot PNG stores (8-bit indexed,
// 64 PLTE entries) and what the strip conversion looks up.
constexpr uint8_t index_of(uint8_t accent, uint8_t level) {
    return static_cast<uint8_t>(((accent & 0x0F) << 2) | (level & 3));
}
constexpr int kPaletteSize = kAccentCount * 4;

// Native RGB565 (R in the high bits; the ST7796 driver sets MADCTL.BGR for
// its BGR-ordered glass, so the panel takes standard RGB565).
constexpr uint16_t rgb565(Rgb c) {
    return static_cast<uint16_t>(((c.r >> 3) << 11) | ((c.g >> 2) << 5) | (c.b >> 3));
}
constexpr uint16_t rgb565(uint8_t v) { return rgb565(gray(v)); }

// SPI panels take the high byte first; esp_lcd sends memory order, so the
// strip holds each pixel byte-swapped.
constexpr uint16_t be16(uint16_t c) {
    return static_cast<uint16_t>((c >> 8) | (c << 8));
}

// One theme's 64 colours, wire-ready. 128 B, built once.
struct Palette565 {
    uint16_t px[kPaletteSize];
};

inline void build(Palette565 &pal, Theme t) {
    for (int a = 0; a < kAccentCount; ++a)
        for (int l = 0; l < 4; ++l)
            pal.px[index_of(static_cast<uint8_t>(a), static_cast<uint8_t>(l))] =
                be16(rgb565(color(t, static_cast<uint8_t>(a), static_cast<uint8_t>(l))));
}

// Expand `rows` rows into `dst` (width*rows pixels). `gray` rows are
// `gstride` bytes apart; `accent` (may be null = all neutral) rows `astride`.
// `width` must be a multiple of 4 (both colour panels are: 320, 368).
inline void expand_rows(const uint8_t *gray, size_t gstride, const uint8_t *accent,
                        size_t astride, int width, int rows, const Palette565 &pal,
                        uint16_t *dst) {
    const int bytes = width / 4;
    for (int r = 0; r < rows; ++r) {
        const uint8_t *g = gray + static_cast<size_t>(r) * gstride;
        const uint8_t *a = accent ? accent + static_cast<size_t>(r) * astride : nullptr;
        for (int x = 0; x < bytes; ++x, dst += 4) {
            const uint8_t gb = g[x];
            // Two accent bytes cover the same four pixels as one gray byte.
            const uint8_t a0 = a ? a[2 * x] : 0, a1 = a ? a[2 * x + 1] : 0;
            dst[0] = pal.px[((a0 >> 4) << 2) | (gb >> 6)];
            dst[1] = pal.px[((a0 & 0x0F) << 2) | ((gb >> 4) & 3)];
            dst[2] = pal.px[((a1 >> 4) << 2) | ((gb >> 2) & 3)];
            dst[3] = pal.px[((a1 & 0x0F) << 2) | (gb & 3)];
        }
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

// Union of two optional bands (ok flags), into *y0/*y1. False = neither.
inline bool band_union(bool ok_a, int a0, int a1, bool ok_b, int b0, int b1,
                       int *y0, int *y1) {
    if (!ok_a && !ok_b) return false;
    if (!ok_a) { *y0 = b0; *y1 = b1; return true; }
    if (!ok_b) { *y0 = a0; *y1 = a1; return true; }
    *y0 = a0 < b0 ? a0 : b0;
    *y1 = a1 > b1 ? a1 : b1;
    return true;
}

// Card grammar: accent name -> index. "none"/"gray"/"neutral" = kNeutral.
// Unknown -> -1 (caller keeps its fallback and logs).
inline int accent_from_name(const char *s) {
    if (s == nullptr) return -1;
    static const char *const kNames[kNamedAccents] = {
        "neutral", "mono", "red", "orange", "yellow",
        "green", "teal", "blue", "purple", "pink"};
    for (int i = 0; i < kNamedAccents; ++i)
        if (std::strcmp(s, kNames[i]) == 0) return i;
    if (std::strcmp(s, "none") == 0 || std::strcmp(s, "gray") == 0 ||
        std::strcmp(s, "grey") == 0)
        return kNeutral;
    if (std::strcmp(s, "cyan") == 0) return kTeal;
    return -1;
}

}  // namespace lcd_palette
