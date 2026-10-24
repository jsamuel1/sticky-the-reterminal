// tiny_led_state — which colour the status light shows, as pure functions.
//
// No ESP-IDF headers: tests/host/led_host_test.sh compiles this file on the
// host and walks the whole table. tiny_led.cpp owns the mutex, the timer and
// the RMT write; this file owns the inputs' transitions and the decision.
//
// Two owners feed the light, each through its own field:
//   capture (tiny_audio)  -> `capturing` while the mic records
//   ask     (tiny_node)   -> `ask`: PENDING (voice ask waiting on its
//                            capture, dark), THINKING (upload or ask in
//                            flight), IDLE
// A voice ask arms PENDING before capture starts; a capture that ends OK
// promotes PENDING to THINKING inside the same transition, so blue goes
// straight to amber with no black frame latched between them.
//
// Two more owners hold the attention layer, which sits under every live
// state above and is steady (no timer) for as long as its cause stands:
//   display (tiny_display) -> `alert`: a priority:"alert" card is on glass
//   node    (tiny_node)     -> `unread`: the heartbeat reports unread DMs > 0
//
// Precedence, strongest first:
//   locked or asleep  -> off     (UX_SPEC §11: a pocket shows nothing)
//   capturing         -> blue    (the mic is capturing)
//   ask THINKING      -> amber   (upload or ask in flight)
//   error, unexpired  -> red     (askfail card / upload refused, a few seconds)
//   alert card shown  -> magenta (until a non-alert card replaces it)
//   unread > 0        -> green   (until the heartbeat reports 0)
//   otherwise         -> off     (includes ask PENDING before capture starts)
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TINY_LED_ASK_IDLE = 0,
    TINY_LED_ASK_PENDING,   // voice ask armed; turns THINKING when capture ends OK
    TINY_LED_ASK_THINKING,
} tiny_led_ask_t;

typedef struct {
    uint8_t r, g, b;
} tiny_led_rgb_t;

typedef struct {
    bool capturing;          // mic capture running
    tiny_led_ask_t ask;      // ask engine state
    int64_t error_until_us;  // red until this uptime; 0 = no error showing
    bool locked;             // pocket lockout engaged
    bool asleep;             // deep sleep is being entered
    bool alert;              // a priority:"alert" card is the committed card
    bool unread;             // the platform last reported unread DMs > 0
} tiny_led_inputs_t;

// Full-scale colours; tiny_led_resolve scales them by brightness/255.
#define TINY_LED_RGB_LISTENING {0, 40, 255}
#define TINY_LED_RGB_THINKING {255, 110, 0}
#define TINY_LED_RGB_ERROR {255, 0, 0}
// Attention colours stay clear of the three live ones: magenta is not red
// at low brightness (its blue channel survives scaling), green is the only
// colour with no red at all.
#define TINY_LED_RGB_ALERT {255, 0, 160}
#define TINY_LED_RGB_UNREAD {0, 255, 40}

// --- transitions (caller holds the light's lock) ---------------------------

// Capture starts: blue, and a new capture supersedes the last failure.
static inline void tiny_led_on_capture_begin(tiny_led_inputs_t *in) {
    in->capturing = true;
    in->error_until_us = 0;
}

// Capture ends. A pending voice ask takes over in the same step when the
// capture produced audio; a failed capture leaves it dark for the caller's
// error report.
static inline void tiny_led_on_capture_end(tiny_led_inputs_t *in, bool ok) {
    in->capturing = false;
    if (ok && in->ask == TINY_LED_ASK_PENDING) in->ask = TINY_LED_ASK_THINKING;
}

// The ask engine reports its state. Starting an ask supersedes the last
// failure. Returns true when an error was cleared (the expiry timer can go).
static inline bool tiny_led_on_ask(tiny_led_inputs_t *in, tiny_led_ask_t a) {
    in->ask = a;
    if (a != TINY_LED_ASK_IDLE && in->error_until_us) {
        in->error_until_us = 0;
        return true;
    }
    return false;
}

// A failure card was painted: the ask is over and red shows until `until`.
static inline void tiny_led_on_error(tiny_led_inputs_t *in, int64_t until_us) {
    in->ask = TINY_LED_ASK_IDLE;
    in->error_until_us = until_us;
}

// --- decision ----------------------------------------------------------------

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
    if (in->capturing) {
        const tiny_led_rgb_t k = TINY_LED_RGB_LISTENING;
        c = k;
    } else if (in->ask == TINY_LED_ASK_THINKING) {
        const tiny_led_rgb_t k = TINY_LED_RGB_THINKING;
        c = k;
    } else if (in->error_until_us > now_us) {
        const tiny_led_rgb_t k = TINY_LED_RGB_ERROR;
        c = k;
    } else if (in->alert) {
        const tiny_led_rgb_t k = TINY_LED_RGB_ALERT;
        c = k;
    } else if (in->unread) {
        const tiny_led_rgb_t k = TINY_LED_RGB_UNREAD;
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
