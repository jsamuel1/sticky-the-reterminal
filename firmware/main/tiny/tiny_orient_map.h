// tiny_orient_map — which way up the UI goes, from the in-plane accelerometer.
//
// Pure and header-only so tests/host/orient_host_test.sh compiles the exact
// table the firmware ships. tiny_orient.cpp picks one map per board.
//
// The accelerometer measures specific force: at rest it points UP (away from
// the ground, out of the glass when the device lies face up). Whichever in-plane
// axis carries most of that vector names the edge that is up, and each board's
// map turns "that axis, that sign" into a canvas rotation (degrees, clockwise,
// tiny_display_set_rotation) plus a pose name for `rotate` / `sensors`.
//
// Canvas rotation -> the panel edge the picture's top sits on (canvas.cpp
// to_physical): 0 = native top, 90 = native right, 180 = native bottom,
// 270 = native left.
#pragma once

#include <cmath>

struct TinyOrientPose {
    int deg;           // canvas rotation for this pose
    const char *name;  // stable token reported by `rotate` (imu_last)
};

// One pose per dominant in-plane axis and sign of the IMU's own x/y.
struct TinyOrientMap {
    TinyOrientPose neg_x, pos_x, neg_y, pos_y;
};

// Below this much in-plane gravity the device is lying flat / face up and no
// edge is meaningfully "up": hold the last decision rather than guess.
constexpr float kTinyOrientFlatG = 0.35F;

// reTerminal (LSM6DS3TR-C). Measured, not derived: the driver rotates every
// framebuffer 180 degrees at push, so the vendor's axis comments describe the
// raw panel, not the picture. Owner-on-hardware calibration (tiny_orient.cpp).
constexpr TinyOrientMap kTinyOrientMapReterminal = {
    {180, "landscape"},
    {0, "landscape-flipped"},
    {270, "portrait-right"},
    {90, "portrait-left"},
};

// AMOLED-1.8 V2 (QMI8658 at 0x6B), native 368x448 portrait.
// Evidence: Waveshare's 16_LVGL_Sqprj demo auto-rotates on chip Y alone,
// between its two landscape rotations, so chip Y runs along the panel's
// short (pixel x) axis. It picks LV_DISP_ROT_90 when acc.y < -0.8 (-Y up), and
// LVGL v8's ROT_90 flush puts the picture's top on the native LEFT edge, so
// -Y = native left, +Y = native right (assuming LVGL v8's sw_rotate turns
// clockwise). Chip X then lies along pixel y; the chip is assumed to sit on
// the back of the PCB (components behind the glass), so its +Z points away
// from the viewer and, right-handed, +X = native TOP edge.
// So: up along +X = native top (0 deg), -X = bottom (180), +Y = right (90),
// -Y = left (270).
// The SIGNS are an inference from vendor code and board layout, not a
// measurement: if the owner's board shows the picture upside down (or sideways
// the wrong way) in a pose, it is this table, and only this table, that is
// wrong. `rotate` reports the raw accel_g to calibrate it from.
constexpr TinyOrientMap kTinyOrientMapAmoled18 = {
    {180, "portrait-flipped"},
    {0, "portrait"},
    {270, "landscape-left"},
    {90, "landscape-right"},
};

// Degrees for this reading, or -1 (name "flat") when the reading says nothing
// about which way is up. Ties go to x, as the reTerminal always did.
inline int tiny_orient_classify(float ax, float ay, const TinyOrientMap &m,
                                const char **name)
{
    if (std::sqrt(ax * ax + ay * ay) < kTinyOrientFlatG) {
        *name = "flat";
        return -1;
    }
    const TinyOrientPose &p = std::fabs(ax) >= std::fabs(ay)
                                  ? (ax < 0 ? m.neg_x : m.pos_x)
                                  : (ay < 0 ? m.neg_y : m.pos_y);
    *name = p.name;
    return p.deg;
}
