#include "tiny/board_caps.h"

#include "sdkconfig.h"
#include "tiny/tiny_ota_util.h"

namespace {

#if defined(CONFIG_TINY_BOARD_RETERMINAL)
constexpr tiny_board_caps_t kCaps = {
    .id = "reterminal",
    .name = "Seeed reTerminal Sticky",
    .display = "800x480 4-gray",
    .display_short = "800x480",
    .eink = true,
    .panel = true,
    .color = false,
    .dark_theme = false,
    .brightness = false,  // e-ink: reflective, no backlight to dim
    .touch = true,
    .mic = true,
    .buzzer = true,
    .speaker = true,  // buzzer chimes; the beacon's historical "speaker" bit
    .battery_gauge = true,
    .battery_voltage = true,
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
// ST7796S LCD (lcd_canvas), FT6336U touch, the GPIO 9 battery ADC and the
// SDMMC 4-bit microSD (sticky_sdcard_sdmmc.cpp) are driven.
// The board has no RTC, IMU, env sensor, fuel gauge or power latch. Mic,
// buzzer tones and speaker are the ES8311 (P4), proven on the owner's unit:
// chimes audible, voice ask round-trips through the agent.
constexpr tiny_board_caps_t kCaps = {
    .id = "fnk0104s",
    .name = "Freenove FNK0104S",
    .display = "320x480 colour LCD",
    .display_short = "320x480",
    .eink = false,
    .panel = true,
    .color = true,  // lcd_canvas accent plane + lcd_palette (light theme)
    .dark_theme = false,
    .brightness = true,  // ST7796 backlight: LEDC duty on GPIO 45
    .touch = true,  // FT6336U, tiny_touch.cpp (P3)
    .mic = true,      // ES8311 ADC, audio_es8311.cpp (P4)
    .buzzer = true,   // sticky_buzzer_* as ES8311 tones (P4)
    .speaker = true,  // ES8311 DAC + SC8002B amp: chimes/RTTTL (P4)
    .battery_gauge = true,  // ADC estimate (tiny_batt_curve): no fuel gauge
    .battery_voltage = true,  // ADC GPIO 9, 2:1 divider (sticky_battery_adc.cpp)
    .rtc = false,
    .imu = false,
    .env = false,
    .sd = true,  // SDMMC 4-bit, no detect pin: present = mounted
    .power_latch = false,
    .page_buttons = false,
    .ota = true,  // image carries a board stamp; tiny_ota refuses foreign ones
    .flash_mb = 16,
};
#elif defined(CONFIG_TINY_BOARD_AMOLED18)
// Phase 2b: CO5300 AMOLED (lcd_canvas) and ES8311 audio code are built;
// P3b: CST820 touch (tiny_touch.cpp). QMI8658 accelerometer
// (sticky_imu_qmi8658.cpp) drives auto-rotate and pocket lock. P7: the
// AXP2101 fuel gauge + ADC (battery %, mV, charging, USB) and the PCF85063A
// wall clock are driven. SD lands in a later phase.
// mic/buzzer/speaker stay false until a board proves them.
constexpr tiny_board_caps_t kCaps = {
    .id = "amoled18",
    .name = "Waveshare ESP32-S3-Touch-AMOLED-1.8 V2",
    .display = "368x448 colour AMOLED",
    .display_short = "368x448",
    .eink = false,
    .panel = true,
    .color = true,       // CO5300 behind lcd_canvas: accent plane + palette
    .dark_theme = true,  // AMOLED: light-on-black, no static white
    .brightness = true,  // CO5300 DCS 0x51 (emission level, no backlight)
    .touch = true,       // CST820 via TCA9554 reset, tiny_touch.cpp (P3b)
    .mic = false,
    .buzzer = false,
    .speaker = false,
    .battery_gauge = true,    // AXP2101 fuel gauge (sticky_pmu_axp2101.cpp)
    .battery_voltage = true,  // AXP2101 VBAT ADC
    .rtc = true,              // PCF85063A (sticky_rtc_pcf85063.cpp)
    .imu = true,         // QMI8658 accel, sticky_imu_qmi8658.cpp
    .env = false,
    .sd = false,
    .power_latch = false,
    .page_buttons = false,
    .ota = true,  // image carries a board stamp; tiny_ota refuses foreign ones
    .flash_mb = 16,
};
#else
#error "no CONFIG_TINY_BOARD_* selected (see main/Kconfig.projbuild)"
#endif

}  // namespace

// The board stamp (tiny/tiny_ota_util.h): IDF's linker script places
// .rodata_custom_desc straight after esp_app_desc_t, so these 32 bytes sit at
// image offset 288 and arrive in the first chunk of any OTA download. The
// section has no KEEP in the linker script, so the component links with
// `-u tiny_board_stamp` (main/CMakeLists.txt) and tiny_ota reads it at run
// time; either alone keeps it alive under --gc-sections.
extern "C" const tiny_board_stamp_t tiny_board_stamp
    __attribute__((section(".rodata_custom_desc"), used)) = {
        {'T', 'I', 'N', 'Y', 'B', 'R', 'D', '1'},
#if defined(CONFIG_TINY_BOARD_RETERMINAL)
        "reterminal",
#elif defined(CONFIG_TINY_BOARD_FNK0104S)
        "fnk0104s",
#elif defined(CONFIG_TINY_BOARD_AMOLED18)
        "amoled18",
#endif
        {0},
};

namespace {
constexpr bool same(const char *a, const char *b) {
    return *a == *b && (*a == 0 || same(a + 1, b + 1));
}
}  // namespace
static_assert(same(kCaps.id,
#if defined(CONFIG_TINY_BOARD_RETERMINAL)
                   "reterminal"
#elif defined(CONFIG_TINY_BOARD_FNK0104S)
                   "fnk0104s"
#else
                   "amoled18"
#endif
                   ),
              "board stamp must equal board_caps id");

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
