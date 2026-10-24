// tiny_brightness — the pure rules behind the `brightness` verb (grammar v22)
// and the settings row. No IDF types: tests/host/brightness_host_test.sh
// compiles this exact header.
//
// Level = percent of the panel's own range (ST7796: LEDC backlight duty,
// CO5300: DCS 0x51 emission). 0 is dark, 100 is full.
#pragma once
#include <stdint.h>

#define TINY_BRIGHTNESS_DEFAULT 80  // lcd_canvas's level before anyone asks
// A level restored at BOOT never goes below this. The colour boards have no
// page buttons and a dark glass reads as a dead board, so a saved 0 must not
// strand the owner. The relay can still set 0 live; status reports the truth.
#define TINY_BRIGHTNESS_BOOT_FLOOR 10

// Settings row stops, in tap order (wraps).
static const uint8_t kTinyBrightnessSteps[] = {25, 50, 80, 100};

typedef enum {
    TINY_BRIGHTNESS_REPORT = 0,  // bare verb (or only spaces): read, change nothing
    TINY_BRIGHTNESS_SET = 1,     // *level holds 0..100
    TINY_BRIGHTNESS_BAD = 2,     // anything else: refuse, change nothing
} tiny_brightness_arg_t;

// Strict parse of the verb argument. Only 1-3 decimal digits worth 0..100,
// optionally surrounded by spaces, set a level. "report", "50%", "-1", "101",
// "5x", "1 2" are refused: atoi() would read most of them as a level.
static inline tiny_brightness_arg_t tiny_brightness_parse(const char *arg,
                                                          uint8_t *level) {
    if (!arg) return TINY_BRIGHTNESS_REPORT;
    while (*arg == ' ') ++arg;
    if (!*arg) return TINY_BRIGHTNESS_REPORT;
    unsigned v = 0;
    int digits = 0;
    while (*arg >= '0' && *arg <= '9') {
        if (++digits > 3) return TINY_BRIGHTNESS_BAD;
        v = v * 10 + (unsigned)(*arg - '0');
        ++arg;
    }
    while (*arg == ' ') ++arg;
    if (digits == 0 || *arg || v > 100) return TINY_BRIGHTNESS_BAD;
    if (level) *level = (uint8_t)v;
    return TINY_BRIGHTNESS_SET;
}

// What the panel is lit at after a boot, from the NVS value (`have` false =
// never written). Out-of-range values (a future firmware's encoding) fall
// back to the default rather than being trusted.
static inline uint8_t tiny_brightness_boot_level(bool have, uint8_t saved) {
    if (!have || saved > 100) return TINY_BRIGHTNESS_DEFAULT;
    return saved < TINY_BRIGHTNESS_BOOT_FLOOR ? TINY_BRIGHTNESS_BOOT_FLOOR : saved;
}

// The settings row's next stop: the first step above `cur`, else the first.
static inline uint8_t tiny_brightness_next_step(uint8_t cur) {
    const int n = (int)(sizeof kTinyBrightnessSteps / sizeof *kTinyBrightnessSteps);
    for (int i = 0; i < n; ++i)
        if (kTinyBrightnessSteps[i] > cur) return kTinyBrightnessSteps[i];
    return kTinyBrightnessSteps[0];
}
