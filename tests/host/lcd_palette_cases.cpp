// lcd_palette host cases: the shipped lcd_palette.h + tiny_png.h against the
// shipped Canvas. What the panel receives must be what the canvas holds —
// checked through real draw_pixel/fill_rect calls, not hand-packed bytes.
// Also writes two PNGs (argv[1] dir) that lcd_palette_host_test.sh decodes
// with an independent decoder.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "canvas.h"
#include "lcd_palette.h"
#include "tiny/tiny_png.h"

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

using namespace lcd_palette;

static uint32_t crc32_bitwise(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

static int level_at(const Canvas &c, int x, int y) {
    return (c.data()[y * c.stride() + x / 4] >> (6 - 2 * (x % 4))) & 3;
}

static bool same(Rgb a, Rgb b) { return a.r == b.r && a.g == b.g && a.b == b.b; }

// Paint one fixed scene. Accent calls are no-ops on a plane-less canvas.
static void scene(Canvas &c) {
    c.clear(GrayLevel::White);
    c.set_pen(kNeutral);
    c.draw_text(4, 4, "hello", 2, GrayLevel::Black);
    c.set_pen(kBlue);
    c.fill_rect(10, 40, 33, 7, GrayLevel::Black);
    c.set_pen(kRed);
    c.draw_line(0, 60, 99, 70, GrayLevel::DarkGray);
    c.set_pen(kGreen);
    c.fill_rect(50, 80, 9, 9, GrayLevel::LightGray);
    c.set_pen(kNeutral);
}

static void write_file(const std::string &path, const uint8_t *p, size_t n) {
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) { ++fails; std::perror(path.c_str()); return; }
    std::fwrite(p, 1, n, f);
    std::fclose(f);
}

int main(int argc, char **argv) {
    const std::string outdir = argc > 1 ? argv[1] : "/tmp";

    // 1. palette: neutral light == the old gray LUT; mono is literal gray in
    //    both themes; dark neutral inverts; inks have the level ordering.
    for (int l = 0; l < 4; ++l) {
        CHECK(same(color(Theme::Light, kNeutral, l), gray(kLevel8[l])), "light neutral %d", l);
        CHECK(same(color(Theme::Light, kMono, l), gray(kLevel8[l])), "light mono %d", l);
        CHECK(same(color(Theme::Dark, kMono, l), gray(kLevel8[l])), "dark mono %d", l);
    }
    CHECK(same(color(Theme::Dark, kNeutral, 3), gray(0x00)), "dark bg is black");
    CHECK(color(Theme::Dark, kNeutral, 0).r > 0xC0, "dark ink is light");
    CHECK(color(Theme::Dark, kNeutral, 0).r < 0xFF, "dark ink is not full white");
    CHECK(same(color(Theme::Light, kBlue, 3), gray(0xFF)), "light accent bg = white");
    CHECK(same(color(Theme::Dark, kBlue, 3), gray(0x00)), "dark accent bg = black");
    CHECK(same(color(Theme::Light, kBlue, 0), kInkLight[kBlue]), "blue ink");
    CHECK(same(color(Theme::Dark, kRed, 0), kInkDark[kRed]), "dark red ink");
    {
        const Rgb ink = color(Theme::Light, kRed, 0), mid = color(Theme::Light, kRed, 1),
                  tint = color(Theme::Light, kRed, 2);
        CHECK(ink.g < mid.g && mid.g < tint.g && tint.g < 0xFF, "red levels ordered");
        CHECK(tint.r > tint.g, "red tint stays red");
    }
    CHECK(same(color(Theme::Light, 12, 0), color(Theme::Light, kNeutral, 0)),
          "unassigned accent draws neutral");
    // Every named accent's ink is distinct from neutral and from each other.
    for (int a = kRed; a < kNamedAccents; ++a)
        for (int b = a + 1; b < kNamedAccents; ++b)
            CHECK(!same(kInkLight[a], kInkLight[b]) && !same(kInkDark[a], kInkDark[b]),
                  "inks %d/%d distinct", a, b);
    CHECK(index_of(kBlue, 2) == ((7 << 2) | 2), "index_of");
    CHECK(rgb565(Rgb{0xFF, 0, 0}) == 0xF800, "rgb565 red high bits");
    CHECK(rgb565(Rgb{0, 0, 0xFF}) == 0x001F, "rgb565 blue low bits");
    CHECK(rgb565(0x00) == 0x0000 && rgb565(0xFF) == 0xFFFF, "rgb565 gray extremes");
    CHECK(be16(0x1234) == 0x3412, "be16");
    CHECK(accent_from_name("blue") == kBlue && accent_from_name("grey") == kNeutral &&
              accent_from_name("cyan") == kTeal && accent_from_name("mono") == kMono &&
              accent_from_name("chartreuse") == -1 && accent_from_name(nullptr) == -1,
          "accent names");

    // 2. the e-ink invariant: a canvas WITHOUT an accent plane produces the
    //    exact same gray bytes whatever pens the renderer sets, as one that
    //    never heard of pens.
    constexpr int EW = 800, EH = 480;
    std::vector<uint8_t> e1(EW / 4 * EH), e2(EW / 4 * EH);
    {
        Canvas a(EW, EH, e1.data(), e1.size());
        scene(a);
        CHECK(!a.has_accent() && a.accent_at(10, 40) == 0, "e-ink: no plane");
        Canvas b(EW, EH, e2.data(), e2.size());
        b.clear(GrayLevel::White);
        b.draw_text(4, 4, "hello", 2, GrayLevel::Black);
        b.fill_rect(10, 40, 33, 7, GrayLevel::Black);
        b.draw_line(0, 60, 99, 70, GrayLevel::DarkGray);
        b.fill_rect(50, 80, 9, 9, GrayLevel::LightGray);
        CHECK(e1 == e2, "e-ink gray plane byte-identical with/without pens");
    }

    // 3. colour canvas: the gray plane is ALSO identical (accent is beside
    //    it, never in it), and the accent plane holds the pens.
    constexpr int W = 320, H = 480;
    std::vector<uint8_t> fb(W / 4 * H), acc(W / 2 * H), ref(W / 4 * H);
    Canvas c(W, H, fb.data(), fb.size());
    c.set_accent_plane(acc.data(), acc.size());
    CHECK(c.has_accent() && c.accent_stride() == W / 2, "plane attached");
    {
        std::vector<uint8_t> tiny(10);
        Canvas t(W, H, ref.data(), ref.size());
        t.set_accent_plane(tiny.data(), tiny.size());
        CHECK(!t.has_accent(), "undersized plane refused");
    }
    scene(c);
    {
        Canvas r(W, H, ref.data(), ref.size());
        r.clear(GrayLevel::White);
        r.draw_text(4, 4, "hello", 2, GrayLevel::Black);
        r.fill_rect(10, 40, 33, 7, GrayLevel::Black);
        r.draw_line(0, 60, 99, 70, GrayLevel::DarkGray);
        r.fill_rect(50, 80, 9, 9, GrayLevel::LightGray);
        CHECK(fb == ref, "colour gray plane == plain gray plane");
    }
    CHECK(c.accent_at(10, 40) == kBlue && c.accent_at(42, 46) == kBlue, "blue rect");
    CHECK(c.accent_at(9, 40) == 0 && c.accent_at(43, 40) == 0, "blue rect edges");
    CHECK(c.accent_at(0, 60) == kRed && c.accent_at(54, 84) == kGreen, "line + tint");
    CHECK(c.accent_at(300, 300) == kNeutral, "untouched = neutral");
    // clear() resets accents; fill_accent sets them all.
    {
        std::vector<uint8_t> f2(fb.size()), a2(acc.size());
        Canvas t(W, H, f2.data(), f2.size());
        t.set_accent_plane(a2.data(), a2.size());
        t.set_pen(kPink);
        t.fill_rect(0, 0, 5, 5, GrayLevel::Black);
        t.clear();
        CHECK(t.accent_at(0, 0) == 0, "clear resets accent");
        t.fill_accent(kMono);
        CHECK(t.accent_at(0, 0) == kMono && t.accent_at(W - 1, H - 1) == kMono, "fill_accent");
        // Rotation: the accent follows the physical pixel the gray did.
        t.clear();
        t.set_rotation(CanvasRotation::PortraitRight);
        t.set_pen(kOrange);
        t.draw_pixel(7, 11, GrayLevel::Black);
        int px = 0, py = 0;
        t.to_physical(7, 11, px, py);
        CHECK(level_at(t, px, py) == 0 && t.accent_at(px, py) == kOrange, "rotated accent");
    }

    // 4. strip expansion: every pixel equals palette[accent][gray], both themes.
    for (int th = 0; th < 2; ++th) {
        const Theme t = th ? Theme::Dark : Theme::Light;
        static Palette565 pal;
        build(pal, t);
        std::vector<uint16_t> px(W * H);
        expand_rows(c.data(), c.stride(), c.accent_data(), c.accent_stride(), W, H, pal,
                    px.data());
        int bad = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                if (px[y * W + x] !=
                    be16(rgb565(color(t, c.accent_at(x, y), level_at(c, x, y)))))
                    ++bad;
        CHECK(bad == 0, "theme %d: %d pixels mismatched", th, bad);
        CHECK(px[40 * W + 10] == be16(rgb565(color(t, kBlue, 0))), "theme %d blue px", th);
        CHECK(px[300 * W + 300] == be16(rgb565(color(t, kNeutral, 3))), "theme %d bg", th);
        // Null accent plane = all neutral (the pre-colour behaviour).
        std::vector<uint16_t> g(W * 2);
        expand_rows(c.data() + 40 * c.stride(), c.stride(), nullptr, 0, W, 2, pal, g.data());
        CHECK(g[10] == be16(rgb565(color(t, kNeutral, 0))), "null plane neutral");
        // A strip of rows 38..45 equals the same rows of the full frame.
        std::vector<uint16_t> strip(W * 8);
        expand_rows(c.data() + 38 * c.stride(), c.stride(),
                    c.accent_data() + 38 * c.accent_stride(), c.accent_stride(), W, 8, pal,
                    strip.data());
        CHECK(std::memcmp(strip.data(), px.data() + 38 * W, strip.size() * 2) == 0,
              "strip == frame rows");
    }

    // 5. dirty band covers an accent-only change (same gray, new colour).
    {
        std::vector<uint8_t> pg(fb), pa(acc);
        int g0, g1, a0, a1, y0 = -1, y1 = -1;
        c.set_pen(kPurple);
        c.fill_rect(10, 40, 33, 7, GrayLevel::Black);   // same gray as before
        c.set_pen(kNeutral);
        const bool g = dirty_band(pg.data(), fb.data(), c.stride(), H, &g0, &g1);
        const bool a = dirty_band(pa.data(), acc.data(), c.accent_stride(), H, &a0, &a1);
        CHECK(!g && a, "recolour: gray clean, accent dirty");
        CHECK(band_union(g, g0, g1, a, a0, a1, &y0, &y1) && y0 == 40 && y1 == 47,
              "band %d..%d", y0, y1);
        c.draw_pixel(5, 200, GrayLevel::Black);
        const bool g2 = dirty_band(pg.data(), fb.data(), c.stride(), H, &g0, &g1);
        CHECK(g2 && band_union(g2, g0, g1, a, a0, a1, &y0, &y1) && y0 == 40 && y1 == 201,
              "union band %d..%d", y0, y1);
        CHECK(!band_union(false, 0, 0, false, 0, 0, &y0, &y1), "no band");
    }

    // 6. PNGs through the shipped encoder; the shell decodes them independently.
    {
        // gray: the e-ink scene, 2-bit.
        const int rb = EW / 4;
        std::vector<uint8_t> raw(EH * (rb + 1));
        Canvas e(EW, EH, e1.data(), e1.size());
        for (int y = 0; y < EH; ++y) {
            raw[y * (rb + 1)] = 0;
            std::memcpy(&raw[y * (rb + 1) + 1], e.data() + y * e.stride(), rb);
        }
        std::vector<uint8_t> png(tiny_png::capacity(raw.size(), 0));
        const size_t n = tiny_png::encode(png.data(), EW, EH, 2, 0, nullptr, 0, raw.data(),
                                          raw.size(), crc32_bitwise);
        CHECK(n <= png.size() && n > raw.size(), "gray png size %zu", n);
        write_file(outdir + "/gray.png", png.data(), n);
    }
    for (int th = 0; th < 2; ++th) {
        const Theme t = th ? Theme::Dark : Theme::Light;
        std::vector<uint8_t> raw(H * (W + 1));
        for (int y = 0; y < H; ++y) {
            raw[y * (W + 1)] = 0;
            for (int x = 0; x < W; ++x)
                raw[y * (W + 1) + 1 + x] = index_of(c.accent_at(x, y), level_at(c, x, y));
        }
        uint8_t plte[3 * kPaletteSize];
        for (int i = 0; i < kPaletteSize; ++i) {
            const Rgb v = color(t, i >> 2, i & 3);
            plte[3 * i] = v.r; plte[3 * i + 1] = v.g; plte[3 * i + 2] = v.b;
        }
        std::vector<uint8_t> png(tiny_png::capacity(raw.size(), kPaletteSize));
        const size_t n = tiny_png::encode(png.data(), W, H, 8, 3, plte, kPaletteSize,
                                          raw.data(), raw.size(), crc32_bitwise);
        CHECK(n <= png.size(), "colour png fits capacity");
        write_file(outdir + (th ? "/colour-dark.png" : "/colour-light.png"), png.data(), n);
    }

    std::printf("lcd_palette: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
