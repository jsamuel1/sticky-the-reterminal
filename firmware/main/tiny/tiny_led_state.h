// tiny_led_state — which colour the status light shows, as a pure function.
//
// No ESP-IDF headers: tests/host/led_host_test.sh compiles this file on the
// host and walks the whole table. tiny_led.cpp owns the inputs (who set what,
// when) and the RMT write; this file owns only the decision.
//
// Precedence, strongest first:
//   locked or asleep  -> off   (UX_SPEC §11: a pocket shows nothing)
//   listening         -> blue  (the mic is capturing)
//   thinking          -> amber (upload or ask in flight)
//   error, unexpired  -> red   (askfail card / upload refused, a few seconds)
//   otherwise         -> off
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TINY_LED_IDLE = 0,
    TINY_LED_LISTENING,
    TINY_LED_THINKING,
} tiny_led_activity_t;

typedef struct {
    uint8_t r, g, b;
} tiny_led_rgb_t;

typedef struct {
    tiny_led_activity_t activity;
    int64_t error_until_us;  // red until this uptime; 0 = no error showing
    bool locked;             // pocket lockout engaged
    bool asleep;             // deep sleep is being entered
} tiny_led_inputs_t;

// Full-scale colours; tiny_led_resolve scales them by brightness/255.
#define TINY_LED_RGB_LISTENING {0, 40, 255}
#define TINY_LED_RGB_THINKING {255, 110, 0}
#define TINY_LED_RGB_ERROR {255, 0, 0}

static inline uint8_t tiny_led_scale(uint8_t c, uint8_t brightness) {
    if (!c || !brightness) return 0;
    const unsigned v = ((unsigned)c * brightness + 127u) / 255u;
    return (uint8_t)(v ? v : 1);  // a lit channel never rounds to dark
}

static inline tiny_led_rgb_t tiny_led_resolve(const tiny_led_inputs_t *in,
                                              int64_t now_us,
                                              uint8_t brightness) {
    tiny_led_rgb_t c = {0, 0, 0};
    if (!in || in->locked || in->asleep) return c;
    if (in->activity == TINY_LED_LISTENING) {
        const tiny_led_rgb_t k = TINY_LED_RGB_LISTENING;
        c = k;
    } else if (in->activity == TINY_LED_THINKING) {
        const tiny_led_rgb_t k = TINY_LED_RGB_THINKING;
        c = k;
    } else if (in->error_until_us > now_us) {
        const tiny_led_rgb_t k = TINY_LED_RGB_ERROR;
        c = k;
    }
    c.r = tiny_led_scale(c.r, brightness);
    c.g = tiny_led_scale(c.g, brightness);
    c.b = tiny_led_scale(c.b, brightness);
    return c;
}

#ifdef __cplusplus
}
#endif
