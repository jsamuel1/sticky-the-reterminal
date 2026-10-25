// Real lcd_canvas.cpp against a fake asynchronous strip panel. Proves a
// transfer that fails after earlier strips reached panel RAM cannot make the
// next partial refresh trust an unchanged software baseline.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include "canvas.h"
#include "lcd_canvas.h"
#include "lcd_palette.h"
#include "sticky_display.h"
#include "tiny/board_caps.h"

namespace {
constexpr int kW = 32;
constexpr int kH = 64;
std::vector<uint16_t> g_panel(static_cast<size_t>(kW) * kH);
int g_draw_calls = 0;
int g_fail_draw = 0;
int g_checks = 0;
int g_fails = 0;

#define CHECK(cond, ...)                                                   \
    do {                                                                   \
        ++g_checks;                                                        \
        if (!(cond)) {                                                     \
            ++g_fails;                                                     \
            std::fprintf(stderr, "FAIL %s:%d ", __FILE__, __LINE__);      \
            std::fprintf(stderr, __VA_ARGS__);                             \
            std::fprintf(stderr, "\n");                                  \
        }                                                                  \
    } while (0)

esp_err_t panel_init(size_t) { return ESP_OK; }
esp_err_t panel_draw(int y0, int y1, const uint16_t *px) {
    const int call = ++g_draw_calls;
    if (g_fail_draw && call == g_fail_draw) return ESP_FAIL;
    std::copy(px, px + static_cast<size_t>(y1 - y0) * kW,
              g_panel.begin() + static_cast<size_t>(y0) * kW);
    return ESP_OK;
}
esp_err_t panel_wait(uint32_t) { return ESP_OK; }
esp_err_t panel_sleep(bool) { return ESP_OK; }
esp_err_t panel_brightness(uint8_t) { return ESP_OK; }

constexpr LcdPanelOps kOps = {
    .name = "fake-strip-panel",
    .width = kW,
    .height = kH,
    .row_align = 2,
    .init = panel_init,
    .draw = panel_draw,
    .wait_done = panel_wait,
    .sleep = panel_sleep,
    .brightness = panel_brightness,
};

const tiny_board_caps_t kCaps = [] {
    tiny_board_caps_t caps{};
    caps.id = "host";
    caps.name = "host";
    caps.display = "fake";
    caps.display_short = "32x64";
    caps.panel = true;
    caps.color = true;
    return caps;
}();
}  // namespace

const LcdPanelOps *lcd_panel_ops() { return &kOps; }
extern "C" const tiny_board_caps_t *tiny_board_caps(void) { return &kCaps; }

int main() {
    CHECK(sticky_display_init() == ESP_OK, "init");
    Canvas *canvas = sticky_display_canvas();
    CHECK(canvas != nullptr, "canvas exists");
    const std::vector<uint8_t> old_gray(canvas->data(),
                                         canvas->data() + canvas->stride() * kH);
    const std::vector<uint8_t> old_accent(
        canvas->accent_data(),
        canvas->accent_data() + canvas->accent_stride() * kH);
    const std::vector<uint16_t> old_panel = g_panel;

    canvas->set_pen(lcd_palette::kPink);
    canvas->clear(GrayLevel::Black);
    canvas->fill_accent(lcd_palette::kPink);
    g_draw_calls = 0;
    g_fail_draw = 3;
    CHECK(sticky_display_refresh() == ESP_FAIL,
          "third strip failure is returned");
    CHECK(g_draw_calls == 3, "failure occurs after two delivered strips (%d)",
          g_draw_calls);
    CHECK(!std::equal(g_panel.begin(), g_panel.begin() + 2 * 16 * kW,
                      old_panel.begin()) &&
              std::equal(g_panel.begin() + 3 * 16 * kW, g_panel.end(),
                         old_panel.begin() + 3 * 16 * kW),
          "failed transfer leaves mixed physical panel");

    std::memcpy(canvas->data(), old_gray.data(), old_gray.size());
    std::memcpy(canvas->accent_data(), old_accent.data(), old_accent.size());
    g_draw_calls = 0;
    g_fail_draw = 0;
    CHECK(sticky_display_refresh_partial() == ESP_OK,
          "partial request repairs uncertain frame");
    CHECK(g_draw_calls == kH / 16,
          "uncertain baseline forces full frame (%d draws)", g_draw_calls);
    CHECK(g_panel == old_panel, "full repair restores exact old physical frame");

    // Once repaired, equal Canvas/baseline may safely no-op again.
    g_draw_calls = 0;
    CHECK(sticky_display_refresh_partial() == ESP_OK && g_draw_calls == 0,
          "successful full repair restores partial no-op baseline");

    std::printf("lcd_canvas: %d/%d checks passed\n", g_checks - g_fails,
                g_checks);
    return g_fails ? 1 : 0;
}
