// brightness host test — the SHIPPED tiny/tiny_brightness.h: strict verb
// parse (no atoi words-as-levels), boot restore floor/default, settings cycle.
#include "tiny/tiny_brightness.h"

#include <cstdio>

static int fails = 0;
#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            ++fails;                                                      \
        }                                                                 \
    } while (0)

static bool sets(const char *a, unsigned want) {
    uint8_t v = 255;
    return tiny_brightness_parse(a, &v) == TINY_BRIGHTNESS_SET && v == want;
}
static bool bad(const char *a) {
    uint8_t v = 77;
    return tiny_brightness_parse(a, &v) == TINY_BRIGHTNESS_BAD && v == 77;
}

int main() {
    // bare verb reports
    CHECK(tiny_brightness_parse("", nullptr) == TINY_BRIGHTNESS_REPORT);
    CHECK(tiny_brightness_parse("   ", nullptr) == TINY_BRIGHTNESS_REPORT);
    CHECK(tiny_brightness_parse(nullptr, nullptr) == TINY_BRIGHTNESS_REPORT);
    // the range, ends included
    CHECK(sets("0", 0));
    CHECK(sets("1", 1));
    CHECK(sets("40", 40));
    CHECK(sets("100", 100));
    CHECK(sets(" 55 ", 55));
    CHECK(sets("007", 7));
    // words, units, signs, overflow: refused, *level untouched
    CHECK(bad("report"));
    CHECK(bad("50%"));
    CHECK(bad("-1"));
    CHECK(bad("+5"));
    CHECK(bad("101"));
    CHECK(bad("255"));
    CHECK(bad("1000"));
    CHECK(bad("0100"));   // 4 digits: refused, not read as 100
    CHECK(bad("5x"));
    CHECK(bad("1 2"));
    CHECK(bad("4.5"));
    CHECK(bad("max"));
    // boot restore
    CHECK(tiny_brightness_boot_level(false, 0) == TINY_BRIGHTNESS_DEFAULT);
    CHECK(tiny_brightness_boot_level(true, 101) == TINY_BRIGHTNESS_DEFAULT);
    CHECK(tiny_brightness_boot_level(true, 255) == TINY_BRIGHTNESS_DEFAULT);
    CHECK(tiny_brightness_boot_level(true, 0) == TINY_BRIGHTNESS_BOOT_FLOOR);
    CHECK(tiny_brightness_boot_level(true, 9) == TINY_BRIGHTNESS_BOOT_FLOOR);
    CHECK(tiny_brightness_boot_level(true, 10) == 10);
    CHECK(tiny_brightness_boot_level(true, 40) == 40);
    CHECK(tiny_brightness_boot_level(true, 100) == 100);
    // settings cycle 25 -> 50 -> 80 -> 100 -> 25; odd relay levels join it
    CHECK(tiny_brightness_next_step(25) == 50);
    CHECK(tiny_brightness_next_step(50) == 80);
    CHECK(tiny_brightness_next_step(80) == 100);
    CHECK(tiny_brightness_next_step(100) == 25);
    CHECK(tiny_brightness_next_step(0) == 25);
    CHECK(tiny_brightness_next_step(37) == 50);
    CHECK(tiny_brightness_next_step(TINY_BRIGHTNESS_DEFAULT) == 100);
    if (fails) {
        std::fprintf(stderr, "%d brightness check(s) failed\n", fails);
        return 1;
    }
    return 0;
}
