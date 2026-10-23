// board_caps — what THIS board physically has, in one table.
//
// The Kconfig board choice picks the pin map and the HAL sources at build
// time; this table carries the same choice into run time so init, `status`,
// the BLE beacon and the auto-sleep policy ask one place instead of each
// growing its own #if. A field is true only when the firmware actually drives
// that part on this board — "the chip is soldered on" is not enough. A phase
// that lands a driver flips its field.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *id;       // stable token: "reterminal" | "fnk0104s" | "amoled18"
    const char *name;     // human name for logs
    const char *display;  // status `display` string
    const char *display_short;  // status summary word ("800x480", "headless")
    bool eink;            // SSD1677 e-paper panel driven
    bool panel;           // any panel driven (false = headless canvas)
    bool touch;           // touch controller driven
    bool mic;             // voice capture available
    bool buzzer;          // feedback tones available
    bool speaker;         // PCM playback available
    bool battery_gauge;   // a real state-of-charge reading exists
    bool rtc;             // battery-backed wall clock
    bool imu;             // accelerometer (orientation + pocket lock)
    bool env;             // temperature/humidity sensor
    bool sd;              // microSD driven
    bool power_latch;     // board powers itself off unless firmware holds a latch
    bool page_buttons;    // physical UP/DOWN buttons
    bool ota;             // the shared OTA channel carries images for this board
    uint32_t flash_mb;    // flash size this image is configured for
} tiny_board_caps_t;

const tiny_board_caps_t *tiny_board_caps(void);

// TinyBeacon v2 capability bits derived from the table above
// (docs/ble-mesh/SPEC.md; reTerminal = 0x391c = fixtures `sticky_full`).
uint32_t tiny_board_beacon_caps(void);

#ifdef __cplusplus
}
#endif
