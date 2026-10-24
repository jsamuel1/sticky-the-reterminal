// led host test — walks tiny_led_state.h, the SHIPPED decision table for the
// FNK0104S status light: precedence, lock/sleep darkness, error expiry, and
// brightness scaling.
#include "tiny/tiny_led_state.h"

#include <cstdio>
#include <cstdlib>

static int fails = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++fails;                                                      \
        }                                                                 \
    } while (0)

static bool dark(tiny_led_rgb_t c) { return !c.r && !c.g && !c.b; }
static bool eq(tiny_led_rgb_t a, tiny_led_rgb_t b) {
    return a.r == b.r && a.g == b.g && a.b == b.b;
}

int main() {
    const tiny_led_rgb_t blue = TINY_LED_RGB_LISTENING;
    const tiny_led_rgb_t amber = TINY_LED_RGB_THINKING;
    const tiny_led_rgb_t red = TINY_LED_RGB_ERROR;
    const int64_t now = 10'000'000;

    // Idle: off.
    tiny_led_inputs_t in = {};
    CHECK(dark(tiny_led_resolve(&in, now, 255)));
    CHECK(dark(tiny_led_resolve(nullptr, now, 255)));

    // Each activity at full scale shows its colour exactly.
    in.activity = TINY_LED_LISTENING;
    CHECK(eq(tiny_led_resolve(&in, now, 255), blue));
    in.activity = TINY_LED_THINKING;
    CHECK(eq(tiny_led_resolve(&in, now, 255), amber));

    // Error: red until error_until_us, then off.
    in.activity = TINY_LED_IDLE;
    in.error_until_us = now + 1;
    CHECK(eq(tiny_led_resolve(&in, now, 255), red));
    CHECK(dark(tiny_led_resolve(&in, now + 1, 255)));  // expiry is exclusive
    CHECK(dark(tiny_led_resolve(&in, now + 5'000'000, 255)));

    // Live activity outranks a stale error.
    in.error_until_us = now + 1'000'000;
    in.activity = TINY_LED_LISTENING;
    CHECK(eq(tiny_led_resolve(&in, now, 255), blue));
    in.activity = TINY_LED_THINKING;
    CHECK(eq(tiny_led_resolve(&in, now, 255), amber));

    // §11: locked or asleep is dark whatever else is true.
    const tiny_led_activity_t acts[] = {TINY_LED_IDLE, TINY_LED_LISTENING,
                                        TINY_LED_THINKING};
    for (tiny_led_activity_t a : acts) {
        for (int lock = 0; lock < 2; ++lock)
            for (int sleep = 0; sleep < 2; ++sleep) {
                if (!lock && !sleep) continue;
                tiny_led_inputs_t x = {};
                x.activity = a;
                x.error_until_us = now + 1'000'000;
                x.locked = lock;
                x.asleep = sleep;
                CHECK(dark(tiny_led_resolve(&x, now, 255)));
                CHECK(dark(tiny_led_resolve(&x, now, 12)));
            }
    }

    // Brightness: zero is dark; low settings stay lit on every lit channel,
    // keep unlit channels at zero, and never exceed the setting.
    in = {};
    in.activity = TINY_LED_THINKING;
    CHECK(dark(tiny_led_resolve(&in, now, 0)));
    for (unsigned b = 1; b <= 64; ++b) {
        const tiny_led_inputs_t ins[] = {
            {TINY_LED_LISTENING, 0, false, false},
            {TINY_LED_THINKING, 0, false, false},
            {TINY_LED_IDLE, now + 1, false, false},
        };
        const tiny_led_rgb_t full[] = {blue, amber, red};
        for (int i = 0; i < 3; ++i) {
            const tiny_led_rgb_t c = tiny_led_resolve(&ins[i], now, (uint8_t)b);
            CHECK((c.r != 0) == (full[i].r != 0));
            CHECK((c.g != 0) == (full[i].g != 0));
            CHECK((c.b != 0) == (full[i].b != 0));
            CHECK(c.r <= b && c.g <= b && c.b <= b);
        }
    }
    // The shipped default (Kconfig 12): amber 255,110,0 -> 12,5,0.
    {
        const tiny_led_rgb_t c = tiny_led_resolve(&in, now, 12);
        CHECK(c.r == 12 && c.g == 5 && c.b == 0);
    }
    CHECK(tiny_led_scale(255, 255) == 255);
    CHECK(tiny_led_scale(1, 1) == 1);
    CHECK(tiny_led_scale(0, 64) == 0);

    if (fails) {
        std::fprintf(stderr, "led host test: %d failure(s)\n", fails);
        return 1;
    }
    return 0;
}
