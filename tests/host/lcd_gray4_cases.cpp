// lcd_gray4 host cases: the shipped lcd_gray4.h against the shipped Canvas.
// What the panel receives must be what the canvas holds — checked through
// real draw_pixel/fill_rect calls, not hand-packed bytes.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "canvas.h"
#include "lcd_gray4.h"

static int fails = 0, checks = 0;
#define CHECK(cond, ...)                                     \
    do {                                                     \
        ++checks;                                            \
        if (!(cond)) {                                       \
            ++fails;                                         \
            std::fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);               \
            std::fprintf(stderr, "\n");                      \
        }                                                    \
    } while (0)

int main() {
    using namespace lcd_gray4;
    // 1. levels: black/white are the RGB565 extremes, sent high byte first.
    CHECK(rgb565(0x00) == 0x0000, "black");
    CHECK(rgb565(0xFF) == 0xFFFF, "white");
    CHECK(be16(0x1234) == 0x3412, "be16");
    CHECK(rgb565(0x55) < rgb565(0xAA), "dark < light");

    static Lut lut;
    build(lut);
    // 0x1B = levels 0,1,2,3 left to right.
    CHECK(lut.quad[0x1B][0] == be16(rgb565(0x00)), "quad[0]");
    CHECK(lut.quad[0x1B][1] == be16(rgb565(0x55)), "quad[1]");
    CHECK(lut.quad[0x1B][2] == be16(rgb565(0xAA)), "quad[2]");
    CHECK(lut.quad[0x1B][3] == be16(rgb565(0xFF)), "quad[3]");

    // 2. expand a real 320x480 canvas: every pixel equals its level's colour.
    constexpr int W = 320, H = 480;
    std::vector<uint8_t> fb(W / 4 * H);
    Canvas c(W, H, fb.data(), fb.size());
    c.clear(GrayLevel::White);
    c.fill_rect(10, 3, 7, 5, GrayLevel::Black);
    c.draw_pixel(0, 0, GrayLevel::DarkGray);
    c.draw_pixel(W - 1, H - 1, GrayLevel::LightGray);
    std::vector<uint16_t> px(W * H);
    expand_rows(c.data(), c.stride(), W, H, lut, px.data());
    int bad = 0;
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            int lvl = (c.data()[y * c.stride() + x / 4] >> (6 - 2 * (x % 4))) & 3;
            if (px[y * W + x] != be16(rgb565(kLevel8[lvl]))) ++bad;
        }
    CHECK(bad == 0, "%d pixels mismatched", bad);
    CHECK(px[0] == be16(rgb565(0x55)), "top-left dark gray");
    CHECK(px[W * H - 1] == be16(rgb565(0xAA)), "bottom-right light gray");
    CHECK(px[3 * W + 10] == 0x0000 && px[3 * W + 9] == 0xFFFF, "rect edge");

    // 3. strip expansion of rows 3..7 equals the same rows of the full frame.
    std::vector<uint16_t> strip(W * 5);
    expand_rows(c.data() + 3 * c.stride(), c.stride(), W, 5, lut, strip.data());
    CHECK(std::memcmp(strip.data(), px.data() + 3 * W, strip.size() * 2) == 0,
          "strip == frame rows");

    // 4. dirty band.
    std::vector<uint8_t> prev(fb);
    int y0 = -1, y1 = -1;
    CHECK(!dirty_band(prev.data(), fb.data(), c.stride(), H, &y0, &y1), "identical");
    c.draw_pixel(100, 200, GrayLevel::Black);
    c.draw_pixel(5, 210, GrayLevel::Black);
    CHECK(dirty_band(prev.data(), fb.data(), c.stride(), H, &y0, &y1), "changed");
    CHECK(y0 == 200 && y1 == 211, "band %d..%d", y0, y1);
    prev = fb;
    c.draw_pixel(0, 0, GrayLevel::White);
    c.draw_pixel(0, H - 1, GrayLevel::Black);
    CHECK(dirty_band(prev.data(), fb.data(), c.stride(), H, &y0, &y1) &&
              y0 == 0 && y1 == H, "full band %d..%d", y0, y1);

    std::printf("lcd_gray4: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
