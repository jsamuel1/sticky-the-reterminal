// tiny_touch_map — pure helpers for the FT6336 path in tiny_touch.cpp.
//
// Header-only and free of ESP-IDF on purpose: tests/host compiles it next to
// the shipped canvas.cpp, so the axis mapping is checked against
// Canvas::to_logical without a board.
//
// Coordinate contract (same as the GT911 path): the touch task produces
// PHYSICAL canvas coordinates, i.e. the screenshot's pixel space and the space
// tiny_display_button_regions() publishes hit-regions in. Canvas::to_logical
// applies the UI rotation afterwards, so this file never sees a rotation.
#pragma once

#include <cstdint>

// ---- FT6336U register frame -------------------------------------------------
// One read of 7 bytes starting at register 0x02:
//   [0] TD_STATUS  bits 3:0 = touch points (0..2; 0x0F/0xFF = no answer)
//   [1] P1_XH      bits 7:6 = event (0 down, 1 up, 2 contact, 3 none),
//                  bits 3:0 = X[11:8]
//   [2] P1_XL      X[7:0]
//   [3] P1_YH      bits 7:4 = touch id, bits 3:0 = Y[11:8]
//   [4] P1_YL      Y[7:0]
//   [5] P1_WEIGHT
//   [6] P1_MISC    bits 7:4 = area
constexpr uint8_t kFt6336RegTdStatus = 0x02;
constexpr int kFt6336FrameLen = 7;

struct Ft6336Point {
    int count;       // 0 = no finger, 1..2 = fingers, -1 = garbage frame
    uint16_t x, y;   // first point, sensor-native
    uint8_t event;   // 0 down, 1 up, 2 contact, 3 none
    uint8_t id;
    uint8_t weight;
};

// Only one finger is used. A second finger is ignored, not treated as a lift,
// so a palm brushing the edge does not cut a swipe short.
inline Ft6336Point ft6336_parse(const uint8_t f[kFt6336FrameLen]) {
    Ft6336Point p = {};
    const uint8_t n = f[0] & 0x0F;
    if (n > 2) {  // 0x0F / 0xFF: controller busy or not answering
        p.count = -1;
        return p;
    }
    p.event = static_cast<uint8_t>(f[1] >> 6);
    p.x = static_cast<uint16_t>(((f[1] & 0x0F) << 8) | f[2]);
    p.id = static_cast<uint8_t>(f[3] >> 4);
    p.y = static_cast<uint16_t>(((f[3] & 0x0F) << 8) | f[4]);
    p.weight = f[5];
    // A "lift up" or "no event" record is not a finger on the glass, whatever
    // TD_STATUS says (it can lag the event bits by one scan).
    p.count = (n == 0 || p.event == 1 || p.event == 3) ? 0 : n;
    return p;
}

// ---- axis mapping -----------------------------------------------------------
// Sensor-native -> physical canvas. Mirrors are applied in sensor space, then
// the optional swap, then a linear scale onto the canvas. The scale is what
// lets the same map serve the phase-1 800x480 headless canvas and a 1:1
// panel-sized canvas: corner goes to corner either way.
struct TouchAxisMap {
    uint16_t sensor_w, sensor_h;  // native report range (FT6336 on FNK: 320x480)
    bool mirror_x, mirror_y;      // in sensor space, before the swap
    bool swap_xy;
};

inline uint16_t touch_map_scale(uint16_t v, uint16_t in_span, uint16_t out_span) {
    // in_span/out_span are counts (width, not max index). Rounds to nearest.
    if (in_span <= 1 || out_span <= 1) return 0;
    const uint32_t in_max = in_span - 1U, out_max = out_span - 1U;
    const uint32_t c = v > in_max ? in_max : v;
    return static_cast<uint16_t>((c * out_max + in_max / 2U) / in_max);
}

inline void touch_map_point(const TouchAxisMap &m, uint16_t rx, uint16_t ry,
                            uint16_t canvas_w, uint16_t canvas_h,
                            uint16_t &px, uint16_t &py) {
    uint16_t x = rx >= m.sensor_w ? static_cast<uint16_t>(m.sensor_w - 1U) : rx;
    uint16_t y = ry >= m.sensor_h ? static_cast<uint16_t>(m.sensor_h - 1U) : ry;
    if (m.mirror_x) x = static_cast<uint16_t>(m.sensor_w - 1U - x);
    if (m.mirror_y) y = static_cast<uint16_t>(m.sensor_h - 1U - y);
    uint16_t sw = m.sensor_w, sh = m.sensor_h;
    if (m.swap_xy) {
        const uint16_t t = x; x = y; y = t;
        sw = m.sensor_h; sh = m.sensor_w;
    }
    px = touch_map_scale(x, sw, canvas_w);
    py = touch_map_scale(y, sh, canvas_h);
}

// THE calibration point for the FNK0104S. The FT6336U reports the ST7796's
// native portrait frame, 320 wide x 480 tall, origin at the panel's own
// (0,0). The canvas the panel driver builds decides the physical frame:
//   * portrait canvas (w < h): the sensor frame IS the physical frame;
//   * landscape canvas (w >= h): the panel is driven turned a quarter
//     (ST7796 MADCTL MV), so physical X runs along the sensor's Y and physical
//     Y runs back along the sensor's X.
// UNVERIFIED ON GLASS until the ST7796 driver (P2) lands. If a tap lands
// mirrored, flip the matching mirror_* flag here and nowhere else; the host
// test (tests/host/touchmap_*) pins the shape of each choice.
inline TouchAxisMap fnk0104s_touch_axis_map(uint16_t canvas_w, uint16_t canvas_h) {
    if (canvas_w < canvas_h) return TouchAxisMap{320, 480, false, false, false};
    return TouchAxisMap{320, 480, /*mirror_x=*/true, /*mirror_y=*/false,
                        /*swap_xy=*/true};
}
