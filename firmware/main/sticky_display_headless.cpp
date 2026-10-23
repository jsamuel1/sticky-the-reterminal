// sticky_display_headless — the display HAL for a board whose panel driver
// has not landed yet (FNK0104S / AMOLED-1.8 phase 1).
//
// The UI still renders: every card is drawn into a real 800x480 gray4 canvas
// in PSRAM, exactly as on the e-ink board, and the `screenshot` verb reads it.
// Only the "send it to glass" step is missing, so every refresh succeeds
// without touching a pin. That keeps tiny_display, the shell and onboarding
// byte-for-byte shared, and the first panel phase replaces this file with a
// flush, not a UI rewrite.
#include "sticky_display.h"

#include <new>

#include "canvas.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace {

constexpr char kTag[] = "sticky_display";
constexpr size_t kFramebufferSize =
    static_cast<size_t>(kStickyDisplayWidth) / 4U * kStickyDisplayHeight;

uint8_t *s_framebuffer = nullptr;
Canvas *s_canvas = nullptr;
uint32_t s_commits = 0;

esp_err_t commit(const char *kind)
{
    if (s_canvas == nullptr) return ESP_ERR_INVALID_STATE;
    // First commit is the proof the render path runs; after that, quiet.
    if (s_commits++ == 0)
        ESP_LOGI(kTag, "headless: first %s refresh rendered to canvas (no panel)", kind);
    return ESP_OK;
}

}  // namespace

esp_err_t sticky_display_init()
{
    if (s_canvas != nullptr) return ESP_OK;
    s_framebuffer = static_cast<uint8_t *>(
        heap_caps_malloc(kFramebufferSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (s_framebuffer == nullptr) return ESP_ERR_NO_MEM;
    s_canvas = new (std::nothrow)
        Canvas(kStickyDisplayWidth, kStickyDisplayHeight, s_framebuffer, kFramebufferSize);
    if (s_canvas == nullptr) {
        heap_caps_free(s_framebuffer);
        s_framebuffer = nullptr;
        return ESP_ERR_NO_MEM;
    }
    s_canvas->clear();
    ESP_LOGI(kTag, "headless canvas %ux%u gray4 in PSRAM (panel driver not built)",
             kStickyDisplayWidth, kStickyDisplayHeight);
    return ESP_OK;
}

Canvas *sticky_display_canvas() { return s_canvas; }

esp_err_t sticky_display_refresh() { return commit("full"); }
esp_err_t sticky_display_refresh_partial() { return commit("partial"); }

esp_err_t sticky_display_refresh_partial_baseline(const uint8_t *prev_canvas)
{
    if (prev_canvas == nullptr) return ESP_ERR_INVALID_STATE;
    return commit("partial");
}

esp_err_t sticky_display_refresh_monochrome() { return commit("monochrome"); }

esp_err_t sticky_display_clear()
{
    if (s_canvas == nullptr) return ESP_ERR_INVALID_STATE;
    s_canvas->clear();
    return ESP_OK;
}

esp_err_t sticky_display_sleep()
{
    return s_canvas == nullptr ? ESP_ERR_INVALID_STATE : ESP_OK;
}
