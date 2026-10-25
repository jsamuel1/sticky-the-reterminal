// tiny_photo_util — the pure half of the photo pipeline (grammar v27):
// fit, JPEG descale choice, area resample and error-diffusion dither.
//
// Header-only and free of ESP-IDF on purpose: tests/host/photo_host_test.sh
// compiles THIS file into a host binary, and the render host test draws
// message thumbnails and the full-screen viewer through the same functions,
// so the pixels the glass shows are the pixels the tests check.
//
// Every image here is 8-bit luma, row-major, no padding (`w * h` bytes).
// Levels out of the ditherer are canvas GrayLevel values: 0 = black ..
// 3 = white. Two-level output (the e-ink's card refresh is monochrome) is
// 0 or 3 only; four-level output uses all of 0..3.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace tiny_photo_util {

// Rec.601 luma in integer math (weights sum to 256).
inline uint8_t luma(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint8_t>((77u * r + 150u * g + 29u * b) >> 8);
}

// Contain-fit a sw x sh image into a bw x bh box, aspect kept. With
// `allow_up` false an image smaller than the box keeps its own size. Never
// returns a zero dimension for a non-empty input.
inline void fit(int sw, int sh, int bw, int bh, bool allow_up, int *ow, int *oh) {
    *ow = 0; *oh = 0;
    if (sw <= 0 || sh <= 0 || bw <= 0 || bh <= 0) return;
    if (!allow_up && sw <= bw && sh <= bh) { *ow = sw; *oh = sh; return; }
    // Compare sw/sh against bw/bh without floating point.
    if ((int64_t)sw * bh >= (int64_t)sh * bw) {   // width-limited
        *ow = bw;
        *oh = (int)(((int64_t)sh * bw + sw / 2) / sw);
    } else {                                      // height-limited
        *oh = bh;
        *ow = (int)(((int64_t)sw * bh + sh / 2) / sh);
    }
    if (*ow < 1) *ow = 1;
    if (*oh < 1) *oh = 1;
}

// TJpgDec descales by 1/2^s, s in 0..3, while decoding. Pick the largest s
// whose output still covers the wanted size on both axes, so the decoder
// does the cheap part of the downscale and the area filter the rest.
inline int choose_scale(int sw, int sh, int want_w, int want_h) {
    int s = 3;
    while (s > 0 && ((sw >> s) < want_w || (sh >> s) < want_h)) --s;
    return s;
}

// One destination row of an area (box-filter) resample. Downscaling averages
// every source pixel the destination pixel covers; upscaling degenerates to
// nearest neighbour (the covered span is one source pixel).
inline void resample_row(const uint8_t *src, int sw, int sh, int dw, int dh,
                         int y, uint8_t *out) {
    int y0 = (int)((int64_t)y * sh / dh);
    int y1 = (int)((int64_t)(y + 1) * sh / dh);
    if (y1 <= y0) y1 = y0 + 1;
    if (y1 > sh) y1 = sh;
    for (int x = 0; x < dw; ++x) {
        int x0 = (int)((int64_t)x * sw / dw);
        int x1 = (int)((int64_t)(x + 1) * sw / dw);
        if (x1 <= x0) x1 = x0 + 1;
        if (x1 > sw) x1 = sw;
        uint32_t sum = 0;
        for (int yy = y0; yy < y1; ++yy) {
            const uint8_t *r = src + (size_t)yy * sw;
            for (int xx = x0; xx < x1; ++xx) sum += r[xx];
        }
        const uint32_t n = (uint32_t)(x1 - x0) * (uint32_t)(y1 - y0);
        out[x] = (uint8_t)((sum + n / 2) / n);
    }
}

// Whole-image resample into `dst` (dw * dh bytes).
inline void resample(const uint8_t *src, int sw, int sh, uint8_t *dst, int dw, int dh) {
    for (int y = 0; y < dh; ++y) resample_row(src, sw, sh, dw, dh, y, dst + (size_t)y * dw);
}

// Floyd–Steinberg, one row at a time, so a caller can feed resampled rows
// straight in without holding the resampled image. Scratch is two error rows
// of int16 with a one-pixel guard each side.
inline size_t dither_scratch_bytes(int w) { return 2u * (size_t)(w + 2) * sizeof(int16_t); }

struct Ditherer {
    int w = 0;
    int levels = 2;      // 2 or 4
    int16_t *cur = nullptr;
    int16_t *nxt = nullptr;

    // `scratch` must hold dither_scratch_bytes(w).
    void begin(int width, int lv, void *scratch) {
        w = width;
        levels = lv == 4 ? 4 : 2;
        cur = static_cast<int16_t *>(scratch);
        nxt = cur + (w + 2);
        memset(cur, 0, 2u * (size_t)(w + 2) * sizeof(int16_t));
    }

    // Quantise one row of luma into GrayLevel values (0..3).
    void row(const uint8_t *in, uint8_t *out) {
        memset(nxt, 0, (size_t)(w + 2) * sizeof(int16_t));
        for (int x = 0; x < w; ++x) {
            int v = (int)in[x] + cur[x + 1];
            if (v < 0) v = 0;
            if (v > 255) v = 255;
            int q, lvl;
            if (levels == 4) {
                lvl = (v + 42) / 85;       // nearest of 0, 85, 170, 255
                if (lvl > 3) lvl = 3;
                q = lvl * 85;
            } else {
                lvl = v >= 128 ? 3 : 0;
                q = v >= 128 ? 255 : 0;
            }
            out[x] = (uint8_t)lvl;
            const int e = v - q;
            cur[x + 2] = (int16_t)(cur[x + 2] + e * 7 / 16);
            nxt[x]     = (int16_t)(nxt[x] + e * 3 / 16);
            nxt[x + 1] = (int16_t)(nxt[x + 1] + e * 5 / 16);
            nxt[x + 2] = (int16_t)(nxt[x + 2] + e / 16);
        }
        int16_t *t = cur; cur = nxt; nxt = t;
    }
};

}  // namespace tiny_photo_util
