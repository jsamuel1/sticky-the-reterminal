// touchmap host test: compiles the SHIPPED firmware/main/tiny/tiny_touch_map.h
// next to the SHIPPED canvas.cpp. It pins the FT6336 frame parser and checks
// that the sensor -> physical map composes with Canvas::to_logical: a finger on
// the sensor pixel under a logical button must resolve inside that button in
// every rotation, on a portrait panel-sized canvas and on the 800x480 one.
#include "canvas.h"
#include "tiny/tiny_touch_map.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int fails = 0;
#define CHECK(name, cond) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); fails++; } \
} while (0)

static void parse_cases() {
    {   // one finger, contact event, x=0x123 y=0x1DF, id 0, weight 0x20
        const uint8_t f[7] = {0x01, 0x81, 0x23, 0x01, 0xDF, 0x20, 0x00};
        const Ft6336Point p = ft6336_parse(f);
        CHECK("parse: count 1", p.count == 1);
        CHECK("parse: x 12-bit", p.x == 0x123);
        CHECK("parse: y 12-bit", p.y == 0x1DF);
        CHECK("parse: event contact", p.event == 2);
    }
    {   const uint8_t f[7] = {0x00, 0xC0, 0, 0, 0, 0, 0};
        CHECK("parse: no finger", ft6336_parse(f).count == 0);
    }
    {   // TD_STATUS still says 1 but the event is lift-up: not a finger
        const uint8_t f[7] = {0x01, 0x41, 0x00, 0x00, 0x10, 0, 0};
        CHECK("parse: lift event is not a finger", ft6336_parse(f).count == 0);
    }
    {   const uint8_t f[7] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
        CHECK("parse: 0xFF frame is garbage", ft6336_parse(f).count == -1);
    }
    {   // two fingers: first point is reported, second ignored
        const uint8_t f[7] = {0x02, 0x80, 0x10, 0x10, 0x20, 0, 0};
        const Ft6336Point p = ft6336_parse(f);
        CHECK("parse: two fingers -> first point", p.count == 2 && p.x == 0x10 && p.y == 0x20);
    }
}

// Sensor point at the centre of a physical pixel: the inverse of the map,
// computed by search so the test does not re-derive the formula it checks.
static bool sensor_for_physical(const TouchAxisMap &m, uint16_t cw, uint16_t ch,
                                int px, int py, uint16_t &sx, uint16_t &sy) {
    int best = 1 << 30;
    for (uint16_t x = 0; x < m.sensor_w; ++x)
        for (uint16_t y = 0; y < m.sensor_h; ++y) {
            uint16_t qx, qy;
            touch_map_point(m, x, y, cw, ch, qx, qy);
            const int d = std::abs(qx - px) + std::abs(qy - py);
            if (d < best) { best = d; sx = x; sy = y; }
        }
    return best <= 2;
}

static void map_cases(uint16_t cw, uint16_t ch, const char *label) {
    static uint8_t buf[800 / 4 * 480];
    Canvas c(cw, ch, buf, sizeof buf);
    const TouchAxisMap m = fnk0104s_touch_axis_map(cw, ch);
    char name[160];

    // Corners -> corners, and every sensor point lands on the canvas.
    uint16_t px, py, qx, qy;
    touch_map_point(m, 0, 0, cw, ch, px, py);
    touch_map_point(m, m.sensor_w - 1, m.sensor_h - 1, cw, ch, qx, qy);
    snprintf(name, sizeof name, "%s: sensor corners span the canvas", label);
    CHECK(name, ((px == 0 && qx == cw - 1) || (px == cw - 1 && qx == 0)) &&
                ((py == 0 && qy == ch - 1) || (py == ch - 1 && qy == 0)));
    bool inside = true;
    for (uint16_t x = 0; x < 4096 && inside; x += 37)
        for (uint16_t y = 0; y < 4096 && inside; y += 41) {
            touch_map_point(m, x, y, cw, ch, px, py);
            inside = px < cw && py < ch;
        }
    snprintf(name, sizeof name, "%s: out-of-range reports clamp onto canvas", label);
    CHECK(name, inside);

    // Sensor long axis follows canvas long axis.
    touch_map_point(m, 0, 0, cw, ch, px, py);
    touch_map_point(m, 0, m.sensor_h - 1, cw, ch, qx, qy);
    const int dlong = (cw >= ch) ? std::abs(qx - px) : std::abs(qy - py);
    snprintf(name, sizeof name, "%s: sensor Y runs along the canvas long axis", label);
    CHECK(name, dlong == (cw >= ch ? cw : ch) - 1);

    // The chain the touch task depends on: logical button -> rect_to_physical
    // (what tiny_display publishes) -> finger on the sensor -> map ->
    // to_logical must land back inside the logical button, in all rotations.
    const CanvasRotation rots[] = {CanvasRotation::Landscape,
                                   CanvasRotation::PortraitRight,
                                   CanvasRotation::LandscapeFlipped,
                                   CanvasRotation::PortraitLeft};
    for (CanvasRotation r : rots) {
        c.set_rotation(r);
        const int lw = c.width(), lh = c.height();
        // A bottom-bar button in the logical layout's right third.
        const int bx = lw * 2 / 3, by = lh - 56, bw = lw / 3 - 8, bh = 48;
        int rx, ry, rw, rh;
        c.rect_to_physical(bx, by, bw, bh, rx, ry, rw, rh);
        const int cx = rx + rw / 2, cy = ry + rh / 2;
        uint16_t sx = 0, sy = 0;
        const bool found = sensor_for_physical(m, cw, ch, cx, cy, sx, sy);
        uint16_t tx, ty;
        touch_map_point(m, sx, sy, cw, ch, tx, ty);
        int lx, ly;
        c.to_logical(tx, ty, lx, ly);
        snprintf(name, sizeof name,
                 "%s rot=%d: finger on button resolves inside it "
                 "(phys %d,%d -> logical %d,%d in %d,%d %dx%d)",
                 label, (int)r, tx, ty, lx, ly, bx, by, bw, bh);
        CHECK(name, found && tx >= rx && tx < rx + rw && ty >= ry &&
                    ty < ry + rh && lx >= bx && lx < bx + bw && ly >= by &&
                    ly < by + bh);
    }
}

int main() {
    parse_cases();
    // Pin today's (pre-glass) calibration: recalibrating is a deliberate edit
    // here AND in fnk0104s_touch_axis_map, never a silent drift.
    {
        uint16_t px, py;
        touch_map_point(fnk0104s_touch_axis_map(320, 480), 0, 0, 320, 480, px, py);
        CHECK("calib portrait: sensor origin -> physical top-left", px == 0 && py == 0);
        touch_map_point(fnk0104s_touch_axis_map(800, 480), 0, 0, 800, 480, px, py);
        CHECK("calib landscape: sensor origin -> physical bottom-left",
              px == 0 && py == 479);
        touch_map_point(fnk0104s_touch_axis_map(800, 480), 319, 479, 800, 480, px, py);
        CHECK("calib landscape: sensor far corner -> physical top-right",
              px == 799 && py == 0);
    }
    map_cases(320, 480, "portrait 320x480");
    map_cases(480, 320, "landscape 480x320");
    map_cases(800, 480, "headless 800x480");
    if (fails) { printf("touchmap: %d FAILED\n", fails); return 1; }
    return 0;
}
