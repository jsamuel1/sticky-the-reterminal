// photo host test — compiles the SHIPPED firmware/main/tiny/tiny_photo_util.h
// (fit, descale choice, area resample, error-diffusion dither) into a host
// binary. The render host test draws thumbnails and the viewer through the
// same header; this file pins the math itself.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "tiny/tiny_photo_util.h"

using namespace tiny_photo_util;

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("PASS "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)

static void fit_is(int sw, int sh, int bw, int bh, bool up, int ew, int eh) {
    int w = -1, h = -1;
    fit(sw, sh, bw, bh, up, &w, &h);
    CHECK(w == ew && h == eh, "fit %dx%d in %dx%d%s -> %dx%d (want %dx%d)",
          sw, sh, bw, bh, up ? " up" : "", w, h, ew, eh);
}

static std::vector<uint8_t> dither(const std::vector<uint8_t> &in, int w, int h, int lv) {
    std::vector<uint8_t> out(in.size());
    std::vector<uint8_t> scratch(dither_scratch_bytes(w));
    Ditherer d;
    d.begin(w, lv, scratch.data());
    for (int y = 0; y < h; ++y) d.row(&in[(size_t)y * w], &out[(size_t)y * w]);
    return out;
}

int main() {
    // ---- luma ----
    CHECK(luma(0, 0, 0) == 0 && luma(255, 255, 255) == 255, "luma black/white endpoints");
    CHECK(luma(0, 255, 0) > luma(255, 0, 0) && luma(255, 0, 0) > luma(0, 0, 255),
          "luma weights green > red > blue");

    // ---- fit (the real thread's two photos, then edges) ----
    fit_is(514, 695, 320, 160, false, 118, 160);    // portrait into a thumb box
    fit_is(1531, 329, 320, 160, false, 320, 69);    // banner into a thumb box
    fit_is(514, 695, 800, 800, false, 514, 695);    // viewer: never enlarged
    fit_is(514, 695, 800, 480, true, 355, 480);     // viewer layout: fills height
    fit_is(1531, 329, 800, 480, true, 800, 172);    // viewer layout: fills width
    fit_is(100, 50, 320, 160, false, 100, 50);      // already small: kept
    fit_is(100, 50, 320, 160, true, 320, 160);      // exact aspect, enlarged
    fit_is(10000, 1, 320, 160, false, 320, 1);      // degenerate: never zero
    fit_is(0, 50, 320, 160, false, 0, 0);           // empty stays empty

    // ---- descale choice: largest 1/2^s that still covers the target ----
    CHECK(choose_scale(4032, 3024, 213, 160) == 3, "12 MP photo -> thumb decodes at 1/8");
    CHECK(choose_scale(514, 695, 118, 160) == 2, "514x695 -> 118x160 decodes at 1/4");
    CHECK(choose_scale(514, 695, 514, 695) == 0, "no downscale -> full decode");
    CHECK(choose_scale(1531, 329, 320, 69) == 2, "banner -> 1/4 (382x82 covers 320x69)");
    for (int sw = 16; sw < 5000; sw += 37) {
        for (int sh = 16; sh < 5000; sh += 53) {
            int w, h;
            fit(sw, sh, 320, 160, false, &w, &h);
            const int s = choose_scale(sw, sh, w, h);
            if ((sw >> s) < w || (sh >> s) < h) {
                CHECK(false, "choose_scale %dx%d -> 1/%d under-covers %dx%d", sw, sh, 1 << s, w, h);
                return 1;
            }
        }
    }
    CHECK(true, "choose_scale always covers the fitted size (sweep)");

    // ---- resample ----
    {   // a 4x4 checker of 2x2 blocks averages to its block values at 2x2
        const uint8_t src[16] = {0, 0, 200, 200, 0, 0, 200, 200,
                                 100, 100, 50, 50, 100, 100, 50, 50};
        uint8_t dst[4];
        resample(src, 4, 4, dst, 2, 2);
        CHECK(dst[0] == 0 && dst[1] == 200 && dst[2] == 100 && dst[3] == 50,
              "box filter averages each covered block");
        uint8_t one;
        resample(src, 4, 4, &one, 1, 1);
        CHECK(one == 88, "1x1 = mean of everything (%d)", one);   // (0+200+100+50)/4 = 87.5
        uint8_t up[64];
        resample(src, 4, 4, up, 8, 8);
        CHECK(up[0] == 0 && up[7] == 200 && up[63] == 50, "upscale is nearest neighbour");
        // degenerate boxes / rows write nothing and do not divide by zero
        uint8_t keep[4] = {7, 7, 7, 7};
        resample_row(src, 4, 4, 0, 2, 0, keep);
        resample_row(src, 4, 4, 2, 0, 0, keep);
        resample_row(src, 0, 4, 2, 2, 0, keep);
        resample_row(src, 4, 4, 2, 2, 2, keep);
        resample_row(src, 4, 4, 2, 2, -1, keep);
        CHECK(keep[0] == 7 && keep[1] == 7 && keep[2] == 7 && keep[3] == 7,
              "resample_row ignores zero-size boxes and out-of-range rows");
    }

    // ---- dither ----
    {
        const int w = 64, h = 64;
        std::vector<uint8_t> flat(w * h);
        for (int v : {0, 255}) {
            std::fill(flat.begin(), flat.end(), (uint8_t)v);
            for (int lv : {2, 4}) {
                auto o = dither(flat, w, h, lv);
                bool same = true;
                for (uint8_t q : o) same = same && q == (v ? 3 : 0);
                CHECK(same, "flat %d stays solid at %d levels", v, lv);
            }
        }
        // Mid gray: two levels -> about half white; four levels -> grays only.
        std::fill(flat.begin(), flat.end(), (uint8_t)128);
        auto two = dither(flat, w, h, 2);
        int white = 0, bad = 0;
        for (uint8_t q : two) { white += q == 3; bad += (q != 0 && q != 3); }
        CHECK(bad == 0, "2-level output is only black/white");
        CHECK(white > w * h * 45 / 100 && white < w * h * 55 / 100,
              "2-level mid gray is ~50%% white (%d of %d)", white, w * h);
        std::fill(flat.begin(), flat.end(), (uint8_t)170);
        auto four = dither(flat, w, h, 4);
        bool all_light = true;
        for (uint8_t q : four) all_light = all_light && q == 2;
        CHECK(all_light, "4-level 170 is exactly LightGray, no noise");
        // A horizontal ramp keeps its mean (error diffusion conserves energy).
        std::vector<uint8_t> ramp(w * h);
        long in_sum = 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) { ramp[y * w + x] = (uint8_t)(x * 4); in_sum += x * 4; }
        for (int lv : {2, 4}) {
            auto o = dither(ramp, w, h, lv);
            long out_sum = 0;
            for (uint8_t q : o) out_sum += q * 85;
            const long diff = labs(out_sum - in_sum) / (w * h);
            CHECK(diff <= 4, "%d-level ramp mean preserved (|d|=%ld)", lv, diff);
        }
        // Width 1 and a guard-pixel probe: no out-of-bounds writes.
        std::vector<uint8_t> scratch(dither_scratch_bytes(1) + 16, 0xAB);
        Ditherer d;
        d.begin(1, 2, scratch.data());
        uint8_t in1 = 200, out1 = 9;
        d.row(&in1, &out1);
        d.row(&in1, &out1);
        bool guard_ok = true;
        for (size_t i = dither_scratch_bytes(1); i < scratch.size(); ++i)
            guard_ok = guard_ok && scratch[i] == 0xAB;
        CHECK(guard_ok && out1 == 3, "1-pixel-wide dither stays inside its scratch");
    }

    if (fails) { printf("%d FAILED\n", fails); return 1; }
    printf("photo util: all checks green\n");
    return 0;
}
