// led host test — walks tiny_led_state.h, the SHIPPED decision table and
// state transitions for the FNK0104S status light: precedence, lock/sleep
// darkness, error expiry, brightness scaling, and the frame sequence each
// real flow (voice ask, text ask, failed capture, miccheck) latches.
#include "tiny/tiny_led_state.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

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

// Models tiny_led.cpp's apply_locked: one resolve per transition, and a write
// only when the colour changes. `frames` is what the WS2812 latches.
struct Light {
    tiny_led_inputs_t in = {};
    std::vector<tiny_led_rgb_t> frames;
    int64_t now = 10'000'000;
    void apply() {
        const tiny_led_rgb_t c = tiny_led_resolve(&in, now, 255);
        if (frames.empty() || !eq(frames.back(), c)) frames.push_back(c);
    }
};

int main() {
    const tiny_led_rgb_t blue = TINY_LED_RGB_LISTENING;
    const tiny_led_rgb_t amber = TINY_LED_RGB_THINKING;
    const tiny_led_rgb_t red = TINY_LED_RGB_ERROR;
    const tiny_led_rgb_t magenta = TINY_LED_RGB_ALERT;
    const tiny_led_rgb_t green = TINY_LED_RGB_UNREAD;
    const tiny_led_rgb_t off = {0, 0, 0};
    const int64_t now = 10'000'000;

    // Idle: off.
    tiny_led_inputs_t in = {};
    CHECK(dark(tiny_led_resolve(&in, now, 255)));
    CHECK(dark(tiny_led_resolve(nullptr, now, 255)));

    // Each owner at full scale shows its colour exactly.
    in.capturing = true;
    CHECK(eq(tiny_led_resolve(&in, now, 255), blue));
    in.capturing = false;
    in.ask = TINY_LED_ASK_THINKING;
    CHECK(eq(tiny_led_resolve(&in, now, 255), amber));
    in.ask = TINY_LED_ASK_PENDING;  // armed, capture not started: dark
    CHECK(dark(tiny_led_resolve(&in, now, 255)));
    in.capturing = true;            // capture outranks any ask state
    CHECK(eq(tiny_led_resolve(&in, now, 255), blue));
    in.ask = TINY_LED_ASK_THINKING;
    CHECK(eq(tiny_led_resolve(&in, now, 255), blue));

    // Error: red until error_until_us, then off.
    in = {};
    in.error_until_us = now + 1;
    CHECK(eq(tiny_led_resolve(&in, now, 255), red));
    CHECK(dark(tiny_led_resolve(&in, now + 1, 255)));  // expiry is exclusive
    CHECK(dark(tiny_led_resolve(&in, now + 5'000'000, 255)));

    // Live activity outranks a stale error.
    in.error_until_us = now + 1'000'000;
    in.capturing = true;
    CHECK(eq(tiny_led_resolve(&in, now, 255), blue));
    in.capturing = false;
    in.ask = TINY_LED_ASK_THINKING;
    CHECK(eq(tiny_led_resolve(&in, now, 255), amber));

    // §11: locked or asleep is dark whatever else is true.
    const tiny_led_ask_t asks[] = {TINY_LED_ASK_IDLE, TINY_LED_ASK_PENDING,
                                   TINY_LED_ASK_THINKING};
    for (tiny_led_ask_t a : asks)
        for (int cap = 0; cap < 2; ++cap)
            for (int lock = 0; lock < 2; ++lock)
                for (int sleep = 0; sleep < 2; ++sleep)
                    for (int att = 0; att < 4; ++att) {
                    if (!lock && !sleep) continue;
                    tiny_led_inputs_t x = {};
                    x.capturing = cap;
                    x.ask = a;
                    x.error_until_us = now + 1'000'000;
                    x.locked = lock;
                    x.asleep = sleep;
                    x.alert = att & 1;
                    x.unread = att & 2;
                    CHECK(dark(tiny_led_resolve(&x, now, 255)));
                    CHECK(dark(tiny_led_resolve(&x, now, 8)));
                }

    // --- attention layer -----------------------------------------------------
    // alert -> magenta, unread -> green, alert outranks unread, and every
    // live state (blue, amber, unexpired red) outranks both.
    {
        tiny_led_inputs_t x = {};
        x.unread = true;
        CHECK(eq(tiny_led_resolve(&x, now, 255), green));
        x.alert = true;
        CHECK(eq(tiny_led_resolve(&x, now, 255), magenta));
        x.unread = false;
        CHECK(eq(tiny_led_resolve(&x, now, 255), magenta));
        for (int att = 1; att < 4; ++att) {
            tiny_led_inputs_t y = {};
            y.alert = att & 1;
            y.unread = att & 2;
            y.capturing = true;
            CHECK(eq(tiny_led_resolve(&y, now, 255), blue));
            y.capturing = false;
            y.ask = TINY_LED_ASK_THINKING;
            CHECK(eq(tiny_led_resolve(&y, now, 255), amber));
            y.ask = TINY_LED_ASK_PENDING;  // pending is dark, so attention shows
            CHECK(eq(tiny_led_resolve(&y, now, 255), (att & 1) ? magenta : green));
            y.ask = TINY_LED_ASK_IDLE;
            y.error_until_us = now + 1;
            CHECK(eq(tiny_led_resolve(&y, now, 255), red));
            // Red expires back to the attention colour, not to off.
            CHECK(eq(tiny_led_resolve(&y, now + 1, 255), (att & 1) ? magenta : green));
        }
        // The five colours are pairwise distinct at the shipped default (8):
        // a scaled magenta must not collapse onto red, nor green onto amber.
        const tiny_led_rgb_t all[] = {blue, amber, red, magenta, green};
        tiny_led_rgb_t scaled[5];
        for (int i = 0; i < 5; ++i) {
            tiny_led_inputs_t s = {};
            if (i == 0) s.capturing = true;
            if (i == 1) s.ask = TINY_LED_ASK_THINKING;
            if (i == 2) s.error_until_us = now + 1;
            if (i == 3) s.alert = true;
            if (i == 4) s.unread = true;
            scaled[i] = tiny_led_resolve(&s, now, 8);
            CHECK(!dark(scaled[i]));
            (void)all;
        }
        for (int i = 0; i < 5; ++i)
            for (int j = i + 1; j < 5; ++j) CHECK(!eq(scaled[i], scaled[j]));
    }

    // Alert card arrives during idle with unread waiting, then a voice ask
    // runs over it and the next (normal) card commit clears it:
    // green -> magenta -> blue -> amber -> magenta -> green.
    {
        Light l;
        l.in.unread = true;                           l.apply();
        l.in.alert = true;                            l.apply();  // alert commit
        tiny_led_on_ask(&l.in, TINY_LED_ASK_PENDING); l.apply();  // stays magenta
        tiny_led_on_capture_begin(&l.in);             l.apply();
        tiny_led_on_capture_end(&l.in, true);         l.apply();
        tiny_led_on_ask(&l.in, TINY_LED_ASK_IDLE);    l.apply();
        l.in.alert = false;                           l.apply();  // normal commit
        CHECK(l.frames.size() == 6);
        if (l.frames.size() == 6) {
            CHECK(eq(l.frames[0], green));
            CHECK(eq(l.frames[1], magenta));
            CHECK(eq(l.frames[2], blue));
            CHECK(eq(l.frames[3], amber));
            CHECK(eq(l.frames[4], magenta));
            CHECK(eq(l.frames[5], green));
        }
        l.in.unread = false;                          l.apply();  // heartbeat 0
        CHECK(eq(l.frames.back(), off));
    }

    // --- transitions -------------------------------------------------------

    // Voice ask, capture OK: off -> blue -> amber -> off. No black frame
    // between blue and amber (the capture's end hands over in one step).
    {
        Light l;
        l.apply();                                         // boot: black
        tiny_led_on_ask(&l.in, TINY_LED_ASK_PENDING); l.apply();
        tiny_led_on_capture_begin(&l.in);             l.apply();
        tiny_led_on_capture_end(&l.in, true);         l.apply();
        tiny_led_on_ask(&l.in, TINY_LED_ASK_IDLE);    l.apply();
        CHECK(l.frames.size() == 4);
        if (l.frames.size() == 4) {
            CHECK(eq(l.frames[0], off));
            CHECK(eq(l.frames[1], blue));
            CHECK(eq(l.frames[2], amber));
            CHECK(eq(l.frames[3], off));
        }
    }

    // Voice ask, upload refused: blue -> amber -> red -> off on expiry.
    {
        Light l;
        l.apply();
        tiny_led_on_ask(&l.in, TINY_LED_ASK_PENDING); l.apply();
        tiny_led_on_capture_begin(&l.in);             l.apply();
        tiny_led_on_capture_end(&l.in, true);         l.apply();
        tiny_led_on_error(&l.in, l.now + 5'000'000);  l.apply();
        CHECK(l.in.ask == TINY_LED_ASK_IDLE);
        tiny_led_on_ask(&l.in, TINY_LED_ASK_IDLE);    l.apply();  // LedGuard
        CHECK(eq(l.frames.back(), red));  // guard's IDLE leaves the red alone
        CHECK(l.in.error_until_us != 0);
        l.now += 5'000'000;                           l.apply();  // timer
        CHECK(l.frames.size() == 5);
        if (l.frames.size() == 5) {
            CHECK(eq(l.frames[1], blue));
            CHECK(eq(l.frames[2], amber));
            CHECK(eq(l.frames[3], red));
            CHECK(eq(l.frames[4], off));
        }
    }

    // Capture fails: blue -> off (pending stays dark) -> red. Never amber.
    {
        Light l;
        l.apply();
        tiny_led_on_ask(&l.in, TINY_LED_ASK_PENDING); l.apply();
        tiny_led_on_capture_begin(&l.in);             l.apply();
        tiny_led_on_capture_end(&l.in, false);        l.apply();
        CHECK(l.in.ask == TINY_LED_ASK_PENDING);
        tiny_led_on_error(&l.in, l.now + 5'000'000);  l.apply();
        for (const tiny_led_rgb_t &f : l.frames) CHECK(!eq(f, amber));
        CHECK(eq(l.frames.back(), red));
    }

    // Text ask: amber -> off, and it clears a showing error (returns true so
    // the caller stops the expiry timer). IDLE never clears one.
    {
        tiny_led_inputs_t x = {};
        x.error_until_us = now + 1'000'000;
        CHECK(!tiny_led_on_ask(&x, TINY_LED_ASK_IDLE));
        CHECK(x.error_until_us != 0);
        CHECK(tiny_led_on_ask(&x, TINY_LED_ASK_THINKING));
        CHECK(x.error_until_us == 0);
        CHECK(eq(tiny_led_resolve(&x, now, 255), amber));
        CHECK(!tiny_led_on_ask(&x, TINY_LED_ASK_THINKING));  // nothing to clear
    }

    // miccheck (capture, no ask): blue -> off, and an OK end does not invent
    // amber. A new capture clears a showing error.
    {
        Light l;
        l.in.error_until_us = l.now + 1'000'000;
        l.apply();
        CHECK(eq(l.frames.back(), red));
        tiny_led_on_capture_begin(&l.in);             l.apply();
        CHECK(l.in.error_until_us == 0);
        tiny_led_on_capture_end(&l.in, true);         l.apply();
        CHECK(l.in.ask == TINY_LED_ASK_IDLE);
        CHECK(l.frames.size() == 3);
        if (l.frames.size() == 3) {
            CHECK(eq(l.frames[1], blue));
            CHECK(eq(l.frames[2], off));
        }
    }

    // --- brightness --------------------------------------------------------
    // Zero is dark; low settings stay lit on every lit channel, keep unlit
    // channels at zero, and never exceed the setting.
    in = {};
    in.ask = TINY_LED_ASK_THINKING;
    CHECK(dark(tiny_led_resolve(&in, now, 0)));
    for (unsigned b = 1; b <= 64; ++b) {
        tiny_led_inputs_t ins[5] = {};
        ins[0].capturing = true;
        ins[1].ask = TINY_LED_ASK_THINKING;
        ins[2].error_until_us = now + 1;
        ins[3].alert = true;
        ins[4].unread = true;
        const tiny_led_rgb_t full[] = {blue, amber, red, magenta, green};
        for (int i = 0; i < 5; ++i) {
            const tiny_led_rgb_t c = tiny_led_resolve(&ins[i], now, (uint8_t)b);
            CHECK((c.r != 0) == (full[i].r != 0));
            CHECK((c.g != 0) == (full[i].g != 0));
            CHECK((c.b != 0) == (full[i].b != 0));
            CHECK(c.r <= b && c.g <= b && c.b <= b);
        }
    }
    // The shipped default (Kconfig 8): amber 255,110,0 -> 8,3,0.
    {
        const tiny_led_rgb_t c = tiny_led_resolve(&in, now, 8);
        CHECK(c.r == 8 && c.g == 3 && c.b == 0);
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
