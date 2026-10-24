// tiny_orient — see tiny/tiny_orient.h.
//
// Why this does NOT use sticky_imu's arrow_* classification: probed live at
// 2026-08-25 23:20Z, the device sitting normally on its stand reports
// orientation "face_up" with accel (x -0.499, y 0.009, z 0.878) — the panel is
// tilted back, so the axis normal to the glass dominates and the vendor
// classifier answers "flat" for the device's ordinary pose. Every arrow_* branch
// would be dead code on a stand.
//
// So we do what a phone does: throw away z and look at the IN-PLANE gravity
// vector (x, y). That measurement is also our calibration reference — in the
// pose that currently looks right-side-up, gravity points along -x. Hence:
//
//      gravity -x  -> 0 deg   (landscape, the reference we measured)
//      gravity +x  -> 180 deg (exactly opposite; certain)
//      gravity -y  -> 90 deg  }  which of the two portraits is which
//      gravity +y  -> 270 deg }  needs a human to turn the device once
//
// The 90/270 handedness is the one thing here that is an assumption rather than
// a measurement (QUESTIONS.md 2026-08-25) — if portrait comes up upside down it
// is this table, and only this table, that is wrong.
//
// The tables themselves live in tiny/tiny_orient_map.h (one per board, host
// tested); this file is the policy: sample, debounce, commit.
#include "tiny/tiny_orient.h"

#include <cstdio>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "sticky_imu.h"
#include "tiny/board_caps.h"
#include "tiny/tiny_admit.h"
#include "tiny/tiny_display.h"
#include "tiny/tiny_lock.h"
#include "tiny/tiny_orient_map.h"

static const char *TAG = "tiny_orient";

#if defined(CONFIG_TINY_BOARD_AMOLED18)
// An AMOLED turn is one ~50 ms push with no flash, so the e-ink's patience
// buys nothing: 3 agreeing samples at 300 ms = the card follows the hand in
// ~1 s, with the same hysteresis against a wobble.
static constexpr int kPollMs = 300;
static constexpr const TinyOrientMap &kMap = kTinyOrientMapAmoled18;
#else
static constexpr int kPollMs = 1000;
static constexpr const TinyOrientMap &kMap = kTinyOrientMapReterminal;
#endif
// 3 agreeing samples ~= 3 s of holding still. An e-ink turn is a visible
// ~600 ms flash, so the cost of being eager is a panel that strobes while the
// human is still moving; the cost of being patient is a second of lag. Patience
// wins on a device you put down.
static constexpr int kAgree = 3;
// tiny_lock's thresholds are sample counts at 1 Hz (tiny_lock.cpp): it is
// fed once per second whatever the rotation poll rate is.
static constexpr int kLockFeedEvery = (1000 + kPollMs - 1) / kPollMs;

static volatile bool s_auto = true;
static int s_last_deg = -1;
static const char *s_last_name = "unknown";
// Instrumentation, not decoration: when the panel did not turn, the only useful
// question is "did the task sample, and what did it see?". Without these the
// answer is a guess (on 2026-08-26 auto-rotate silently stopped
// committing and every theory about why was equally plausible).
static uint32_t s_samples = 0;      // successful IMU reads
static uint32_t s_errors = 0;       // failed IMU reads
static esp_err_t s_last_err = ESP_OK;
static uint32_t s_commits = 0;      // rotations actually applied
static int s_agree = 0;             // consecutive agreeing samples right now
static float s_ax = 0.0F, s_ay = 0.0F, s_az = 0.0F;

// -1 when the reading says nothing about which way is up.
// CALIBRATION (reTerminal P0 bug, owner-on-hardware ~00:20Z: "turns correctly
// but I see upside down"): the original mapping (ax<0 -> 0, ax>0 -> 180, ay<0
// -> 90, ay>0 -> 270) was read off sticky_imu.cpp's vendor axis comments — but
// the DRIVER already rotates every framebuffer 180 degrees at push
// (rotate_framebuffer_180, unconditional in both refresh paths of
// sticky_display.cpp). Those vendor comments describe the raw panel, not the
// picture the user sees; the panel therefore tracked his hand perfectly while
// showing the sky at his feet. Both pairs swap. The owner's report is the
// calibration measurement: constant offset, correct tracking = mapping, not
// logic. kTinyOrientMapReterminal carries the swapped table.
static int classify(const StickyImuState &s, const char **name) {
    return tiny_orient_classify(s.acceleration_x_g, s.acceleration_y_g, kMap,
                                name);
}

static void orient_task(void *) {
    int pending = -1;
    int agree = 0;
    int feed = 0;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        // The IMU is read EVERY poll regardless of auto-rotate: this task is
        // also the §11 auto-lock sensor (tiny_lock_imu_feed below). Rotation
        // policy gates further down; the read does not.
        StickyImuState st = {};
        const esp_err_t rerr = sticky_imu_read(st);
        if (rerr != ESP_OK || !st.valid) {
            s_last_err = rerr != ESP_OK ? rerr : ESP_ERR_INVALID_RESPONSE;
            if ((++s_errors % 30) == 1)   // 1-in-30: a dead IMU must show up in
                                          // the log without drowning it
                ESP_LOGW(TAG, "imu read failed (%lu so far): %s",
                         (unsigned long)s_errors, esp_err_to_name(s_last_err));
            continue;
        }
        ++s_samples;
        s_ax = st.acceleration_x_g;
        s_ay = st.acceleration_y_g;
        s_az = st.acceleration_z_g;
        if (++feed >= kLockFeedEvery) {
            feed = 0;
            tiny_lock_imu_feed(s_ax, s_ay, s_az);
        }

        // While locked, the panel must NOT strobe with pocket movement — a
        // rotation is a visible flash and a battery cost with nobody looking
        // The feed above still runs; only rotation sleeps.
        if (tiny_lock_is_locked() || !s_auto) { pending = -1; agree = 0; continue; }

        const char *name = "unknown";
        const int deg = classify(st, &name);
        if (deg < 0) { pending = -1; agree = 0; continue; }

        if (deg == pending) {
            ++agree;
        } else {
            pending = deg;
            agree = 1;
        }
        s_agree = agree;
        if (agree < kAgree) continue;

        // Record the decision BEFORE the early-out: "already there" is still a
        // reading, and `rotate` reporting imu_last=unknown while the task is
        // happily sampling is a status field lying about its own provenance.
        s_last_deg = deg;
        s_last_name = name;
        if (deg == tiny_display_rotation()) continue;   // already there
        // A rotation commit repaints the card: work, never under a claimed
        // sleep (tiny_admit.h). Refused = not committed; the next agreeing
        // sample after wake tries again.
        tiny_admit::Work work;
        if (!work) continue;
        ++s_commits;
        ESP_LOGI(TAG, "gravity (%.2f, %.2f) g held for %d samples -> %s (%d deg)",
                 st.acceleration_x_g, st.acceleration_y_g, agree, name, deg);
        const esp_err_t err = tiny_display_set_rotation(deg);
        if (err != ESP_OK) ESP_LOGW(TAG, "rotate failed: %s", esp_err_to_name(err));
    }
}

extern "C" esp_err_t tiny_orient_start(void) {
    // 6 KB: the commit path runs tiny_display_set_rotation on THIS task, which
    // parses and draws a whole card (cJSON + text/QR primitives) — the display
    // mutex serialises it against other renderers but the stack is ours.
    return xTaskCreate(orient_task, "orient", 6144, nullptr, 3, nullptr) == pdPASS
               ? ESP_OK
               : ESP_ERR_NO_MEM;
}

extern "C" bool tiny_orient_auto(void) { return s_auto; }

extern "C" void tiny_orient_set_auto(bool on) {
    // No accelerometer = nothing to follow. Reporting "auto" there would be a
    // mode that never acts (FNK0104S), so
    // the request is refused into manual and the log says why.
    if (on && !tiny_board_caps()->imu) {
        ESP_LOGI(TAG, "auto-rotate refused: board has no IMU (manual)");
        s_auto = false;
        return;
    }
    s_auto = on;
    ESP_LOGI(TAG, "auto-rotate %s", on ? "on" : "off");
}

extern "C" int tiny_orient_last_degrees(void) { return s_last_deg; }
extern "C" const char *tiny_orient_last_name(void) { return s_last_name; }

extern "C" int tiny_orient_debug_json(char *out, size_t len) {
    return snprintf(out, len,
                    "{\"samples\":%lu,\"errors\":%lu,\"last_err\":\"%s\","
                    "\"commits\":%lu,\"agree\":%d,"
                    "\"accel_g\":[%.3f,%.3f,%.3f]}",
                    (unsigned long)s_samples, (unsigned long)s_errors,
                    esp_err_to_name(s_last_err), (unsigned long)s_commits,
                    s_agree, s_ax, s_ay, s_az);
}
