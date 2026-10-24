// lcd_band host cases: the shipped lcd_band.h. The CO5300 rejects odd window
// edges, so every partial-refresh band must leave here even, never narrower,
// and inside a panel whose height is a multiple of the alignment.
#include <cstdio>

#include "lcd_band.h"

static int fails = 0, checks = 0;
#define CHECK(cond, ...)                                             \
    do {                                                             \
        ++checks;                                                    \
        if (!(cond)) {                                               \
            ++fails;                                                 \
            std::fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__); \
            std::fprintf(stderr, __VA_ARGS__);                       \
            std::fprintf(stderr, "\n");                              \
        }                                                            \
    } while (0)

static void band(int a, int y0, int y1, int want0, int want1, const char *what) {
    lcd_band::align(a, &y0, &y1);
    CHECK(y0 == want0 && y1 == want1, "%s: got %d..%d want %d..%d", what, y0, y1,
          want0, want1);
}

int main() {
    band(2, 201, 211, 200, 212, "odd edges widen outward");
    band(2, 200, 212, 200, 212, "aligned band unchanged");
    band(2, 333, 334, 332, 334, "one-row change");
    band(2, 0, 1, 0, 2, "first row");
    band(2, 447, 448, 446, 448, "last row of 448 stays in range");
    band(1, 7, 9, 7, 9, "align 1 is a no-op");
    band(0, 7, 9, 7, 9, "align 0 is a no-op");
    band(4, 5, 6, 4, 8, "align 4");

    // Exhaustive at 448 rows / 2: every band is even, covers the input and
    // stays inside the panel.
    int bad = 0;
    for (int y0 = 0; y0 < 448; ++y0)
        for (int y1 = y0 + 1; y1 <= 448; ++y1) {
            int a = y0, b = y1;
            lcd_band::align(2, &a, &b);
            if (a % 2 || b % 2 || a > y0 || b < y1 || a < 0 || b > 448) ++bad;
        }
    CHECK(bad == 0, "%d bands misaligned", bad);

    std::printf("lcd_band: %d/%d checks passed\n", checks - fails, checks);
    return fails ? 1 : 0;
}
