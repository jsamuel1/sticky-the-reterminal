#include "tiny/board_caps.h"

#include "sdkconfig.h"

namespace {

#if defined(CONFIG_TINY_BOARD_RETERMINAL)
constexpr tiny_board_caps_t kCaps = {
    .id = "reterminal",
    .name = "Seeed reTerminal Sticky",
    .display = "800x480 4-gray",
    .display_short = "800x480",
    .eink = true,
    .panel = true,
    .touch = true,
    .mic = true,
    .buzzer = true,
    .speaker = true,  // buzzer chimes; the beacon's historical "speaker" bit
    .battery_gauge = true,
    .rtc = true,
    .imu = true,
    .env = true,
    .sd = true,
    .power_latch = true,
    .page_buttons = true,
    .ota = true,
    .flash_mb = 32,
};
#elif defined(CONFIG_TINY_BOARD_FNK0104S)
// Phase 1: only the radio and the BOOT button are real. ST7796, FT6336,
// ES8311, battery ADC and SDMMC land in later phases and flip their fields.
// The board has no RTC, IMU, env sensor, fuel gauge, buzzer or power latch.
constexpr tiny_board_caps_t kCaps = {
    .id = "fnk0104s",
    .name = "Freenove FNK0104S",
    .display = "none (phase 1 headless canvas)",
    .display_short = "headless",
    .eink = false,
    .panel = false,
    .touch = true,  // FT6336U, tiny_touch.cpp (P3)
    .mic = false,
    .buzzer = false,
    .speaker = false,
    .battery_gauge = false,
    .rtc = false,
    .imu = false,
    .env = false,
    .sd = false,
    .power_latch = false,
    .page_buttons = false,
    .ota = false,
    .flash_mb = 16,
};
#elif defined(CONFIG_TINY_BOARD_AMOLED18)
// Phase 1: radio, BOOT button and the TCA9554 rails. CO5300, CST820,
// ES8311, AXP2101, PCF85063A, QMI8658 and SD land in later phases.
constexpr tiny_board_caps_t kCaps = {
    .id = "amoled18",
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.8 V2",
    .display = "none (phase 1 headless canvas)",
    .display_short = "headless",
    .eink = false,
    .panel = false,
    .touch = false,
    .mic = false,
    .buzzer = false,
    .speaker = false,
    .battery_gauge = false,
    .rtc = false,
    .imu = false,
    .env = false,
    .sd = false,
    .power_latch = false,
    .page_buttons = false,
    .ota = false,
    .flash_mb = 16,
};
#else
#error "no CONFIG_TINY_BOARD_* selected (see main/Kconfig.projbuild)"
#endif

}  // namespace

extern "C" const tiny_board_caps_t *tiny_board_caps(void) { return &kCaps; }

extern "C" uint32_t tiny_board_beacon_caps(void) {
    // Bit positions from docs/ble-mesh/SPEC.md.
    uint32_t c = 1u << 8;  // wifi: every board in this tree
    if (kCaps.speaker) c |= 1u << 2;
    if (kCaps.panel) c |= 1u << 3;
    if (kCaps.imu) c |= 1u << 4;
    if (kCaps.env) c |= 1u << 11;
    if (kCaps.touch) c |= 1u << 12;
    if (kCaps.eink) c |= 1u << 13;
    return c;
}
