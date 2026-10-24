// orient host test: the shipped per-board orientation tables
// (firmware/main/tiny/tiny_orient_map.h) against the shipped Canvas.
//
// For each board the test states, independently of the table, which PHYSICAL
// panel edge is up when gravity's reaction points along each IMU axis. It then
// classifies that reading with the shipped table, applies the rotation to a
// real Canvas, and checks the picture's top row lands on that edge. A sign
// flipped in the table fails here before it reaches a hand.
#include "canvas.h"
#include "tiny/tiny_orient_map.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int fails = 0;
#define CHECK(cond, ...) do { \
    if (cond) { printf("ok   "); printf(__VA_ARGS__); printf("\n"); } \
    else { printf("FAIL "); printf(__VA_ARGS__); printf("\n"); fails++; } \
} while (0)

enum Edge { Top, Right, Bottom, Left };
static const char *edge_name(Edge e) {
    static const char *n[] = {"top", "right", "bottom", "left"};
    return n[e];
}

// Physical edge holding the picture's top-centre pixel at this rotation.
static Edge picture_top_edge(uint16_t pw, uint16_t ph, int deg) {
    std::vector<uint8_t> buf((pw + 3) / 4 * ph, 0xFF);
    Canvas c(pw, ph, buf.data(), buf.size());
    c.set_rotation(static_cast<CanvasRotation>(deg / 90));
    int px = 0, py = 0;
    c.to_physical(c.width() / 2, 0, px, py);
    if (py == 0) return Top;
    if (py == ph - 1) return Bottom;
    if (px == 0) return Left;
    return Right;
}

struct AxisCase {
    float ax, ay;
    Edge up;  // physical edge that is up for this reading (board geometry)
};

static void board(const char *tag, const TinyOrientMap &m, uint16_t pw,
                  uint16_t ph, const AxisCase (&cases)[4]) {
    bool used[4] = {};
    for (const AxisCase &k : cases) {
        const char *name = nullptr;
        const int deg = tiny_orient_classify(k.ax, k.ay, m, &name);
        CHECK(deg >= 0 && deg % 90 == 0 && deg < 360,
              "%s: (%+.1f,%+.1f) -> %d deg (%s)", tag, k.ax, k.ay, deg, name);
        if (deg < 0) continue;
        used[deg / 90] = true;
        const Edge got = picture_top_edge(pw, ph, deg);
        CHECK(got == k.up, "%s: (%+.1f,%+.1f) picture top on %s edge, want %s",
              tag, k.ax, k.ay, edge_name(got), edge_name(k.up));
        CHECK(name && *name && strcmp(name, "flat") != 0, "%s: pose has a name", tag);
    }
    CHECK(used[0] && used[1] && used[2] && used[3],
          "%s: the four poses use all four rotations", tag);

    const char *name = nullptr;
    CHECK(tiny_orient_classify(0.1F, -0.2F, m, &name) == -1 &&
              strcmp(name, "flat") == 0,
          "%s: in-plane 0.22 g (lying flat) holds: -1/flat", tag);
    // Tilted on a stand: z dominates, but the in-plane vector still decides.
    const int tilt = tiny_orient_classify(cases[0].ax * 0.5F, 0.0F, m, &name);
    const int full = tiny_orient_classify(cases[0].ax, 0.0F, m, &name);
    CHECK(tilt == full, "%s: 0.5 g in-plane (stand tilt) matches upright", tag);
    // Exact tie goes to x.
    const int tie = tiny_orient_classify(cases[0].ax * 0.7F, 0.7F, m, &name);
    CHECK(tie == full, "%s: |x|==|y| tie resolves on x", tag);
}

int main() {
    // reTerminal: the owner's calibration (tiny_orient.cpp). Reading -x on the
    // stand = right side up; the driver flips every frame 180 at push, so the
    // CANVAS edge that is up is the bottom one. Table pinned as measured.
    const AxisCase reterminal[4] = {
        {-1.0F, 0.0F, Bottom}, {1.0F, 0.0F, Top},
        {0.0F, -1.0F, Left}, {0.0F, 1.0F, Right},
    };
    board("reterminal", kTinyOrientMapReterminal, 800, 480, reterminal);

    // AMOLED-1.8 V2 (tiny_orient_map.h): +X = native top, -X = bottom,
    // +Y = right, -Y = left.
    const AxisCase amoled[4] = {
        {1.0F, 0.0F, Top}, {-1.0F, 0.0F, Bottom},
        {0.0F, 1.0F, Right}, {0.0F, -1.0F, Left},
    };
    board("amoled18", kTinyOrientMapAmoled18, 368, 448, amoled);

    if (fails) { printf("%d check(s) failed\n", fails); return 1; }
    return 0;
}
