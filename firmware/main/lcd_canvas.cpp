// lcd_canvas — sticky_display_* for colour panels. See lcd_canvas.h for the
// refresh mapping and the canvas-size decision; this file is board-neutral and
// talks to the glass only through lcd_panel_ops().
#include "lcd_canvas.h"

#include <cstring>
#include <new>

#include "canvas.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lcd_gray4.h"
#include "sticky_display.h"

namespace {

constexpr char kTag[] = "lcd_canvas";
// Rows per DMA strip. Two strips live in internal DMA RAM (320 px: 2 x 10 KB),
// the scarcest memory on the board; 16 rows keeps a full frame at 30 pushes.
constexpr int kStripRows = 16;
constexpr uint32_t kDrawTimeoutMs = 1000;

const LcdPanelOps *s_ops = nullptr;
Canvas *s_canvas = nullptr;
uint8_t *s_fb = nullptr;        // the canvas the UI draws into (PSRAM)
uint8_t *s_pushed = nullptr;    // copy of the last frame on the glass (PSRAM)
uint16_t *s_strip[2] = {nullptr, nullptr};
lcd_gray4::Lut *s_lut = nullptr;
size_t s_fb_len = 0;
bool s_awake = false;           // panel out of SLPIN and backlight lit
bool s_slept = false;           // sticky_display_sleep() sent SLPIN
uint8_t s_brightness = 80;
uint32_t s_pushes = 0;

// Push canvas rows [y0, y1) through the two bounce buffers: strip k converts
// while strip k-1 is on the wire. Completions arrive FIFO, so before reusing a
// buffer one wait_done() retires exactly the transfer that last used it.
esp_err_t push_rows(int y0, int y1)
{
    const int w = s_ops->width;
    const size_t stride = s_canvas->stride();
    bool inflight[2] = {false, false};
    esp_err_t err = ESP_OK;
    int k = 0;
    for (int y = y0; y < y1 && err == ESP_OK; y += kStripRows, ++k) {
        const int b = k & 1;
        if (inflight[b]) {
            err = s_ops->wait_done(kDrawTimeoutMs);
            inflight[b] = false;
            if (err != ESP_OK) break;
        }
        const int rows = (y1 - y < kStripRows) ? (y1 - y) : kStripRows;
        lcd_gray4::expand_rows(s_fb + static_cast<size_t>(y) * stride, stride, w,
                               rows, *s_lut, s_strip[b]);
        err = s_ops->draw(y, y + rows, s_strip[b]);
        inflight[b] = (err == ESP_OK);
    }
    // Drain: the canvas may be redrawn the moment we return.
    for (int b = 0; b < 2; ++b) {
        if (!inflight[b]) continue;
        const esp_err_t e = s_ops->wait_done(kDrawTimeoutMs);
        if (err == ESP_OK) err = e;
    }
    return err;
}

esp_err_t wake()
{
    if (!s_slept) return ESP_OK;   // init already left SLPIN
    ESP_RETURN_ON_ERROR(s_ops->sleep(false), kTag, "panel wake");
    s_slept = false;
    return ESP_OK;
}

// Rows [y0, y1) are now what the panel shows; light it if it was dark.
esp_err_t commit(int y0, int y1, const char *kind)
{
    if (s_canvas == nullptr) return ESP_ERR_INVALID_STATE;
    ESP_RETURN_ON_ERROR(wake(), kTag, "wake");
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = (y1 > y0) ? push_rows(y0, y1) : ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "%s push rows %d..%d failed: %s", kind, y0, y1,
                 esp_err_to_name(err));
        return err;
    }
    const size_t stride = s_canvas->stride();
    memcpy(s_pushed + static_cast<size_t>(y0) * stride,
           s_fb + static_cast<size_t>(y0) * stride,
           static_cast<size_t>(y1 - y0) * stride);
    if (!s_awake) {
        err = s_ops->brightness(s_brightness);
        if (err != ESP_OK) return err;
        s_awake = true;
    }
    // First push is the proof the glass path runs; then only slow ones.
    const int64_t us = esp_timer_get_time() - t0;
    if (s_pushes++ == 0 || us > 250000)
        ESP_LOGI(kTag, "%s refresh rows %d..%d in %lld ms", kind, y0, y1,
                 static_cast<long long>(us / 1000));
    return ESP_OK;
}

esp_err_t full(const char *kind) { return commit(0, s_ops->height, kind); }

esp_err_t partial(const char *kind)
{
    if (s_canvas == nullptr) return ESP_ERR_INVALID_STATE;
    int y0 = 0, y1 = 0;
    if (!lcd_gray4::dirty_band(s_pushed, s_fb, s_canvas->stride(), s_ops->height,
                               &y0, &y1)) {
        // Nothing changed; still make sure a sleeping panel comes back.
        return commit(0, 0, kind);
    }
    return commit(y0, y1, kind);
}

void release()
{
    delete s_canvas;
    s_canvas = nullptr;
    heap_caps_free(s_fb);
    heap_caps_free(s_pushed);
    heap_caps_free(s_strip[0]);
    heap_caps_free(s_strip[1]);
    heap_caps_free(s_lut);
    s_fb = s_pushed = nullptr;
    s_strip[0] = s_strip[1] = nullptr;
    s_lut = nullptr;
}

}  // namespace

esp_err_t sticky_display_init()
{
    if (s_canvas != nullptr) return ESP_OK;
    s_ops = lcd_panel_ops();
    if (s_ops == nullptr || s_ops->width % 4 != 0) return ESP_ERR_INVALID_STATE;
    const int w = s_ops->width, h = s_ops->height;
    const size_t strip_bytes = static_cast<size_t>(w) * kStripRows * 2;

    s_fb_len = static_cast<size_t>(w) / 4 * h;
    s_fb = static_cast<uint8_t *>(
        heap_caps_malloc(s_fb_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_pushed = static_cast<uint8_t *>(
        heap_caps_malloc(s_fb_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    s_lut = static_cast<lcd_gray4::Lut *>(
        heap_caps_malloc(sizeof(lcd_gray4::Lut), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    for (auto &s : s_strip)
        s = static_cast<uint16_t *>(heap_caps_malloc(
            strip_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (!s_fb || !s_pushed || !s_lut || !s_strip[0] || !s_strip[1]) {
        release();
        return ESP_ERR_NO_MEM;
    }
    s_canvas = new (std::nothrow) Canvas(w, h, s_fb, s_fb_len);
    if (s_canvas == nullptr) {
        release();
        return ESP_ERR_NO_MEM;
    }
    lcd_gray4::build(*s_lut);
    s_canvas->clear();

    esp_err_t err = s_ops->init(strip_bytes);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "%s init failed: %s", s_ops->name, esp_err_to_name(err));
        release();
        return err;
    }
    // Panel RAM is random after power-on: paint white before the backlight
    // ever comes up, so the first thing a human sees is a clean page.
    s_awake = false;
    err = push_rows(0, h);
    if (err == ESP_OK) memcpy(s_pushed, s_fb, s_fb_len);
    ESP_LOGI(kTag, "%s %dx%d gray4 canvas, %d-row DMA strips (%u B x2): %s",
             s_ops->name, w, h, kStripRows, static_cast<unsigned>(strip_bytes),
             esp_err_to_name(err));
    if (err != ESP_OK) release();
    return err;
}

Canvas *sticky_display_canvas() { return s_canvas; }

esp_err_t sticky_display_refresh() { return full("full"); }
esp_err_t sticky_display_refresh_monochrome() { return full("mono"); }
esp_err_t sticky_display_refresh_partial() { return partial("partial"); }

esp_err_t sticky_display_refresh_partial_baseline(const uint8_t *prev_canvas)
{
    // The e-ink needs the caller's idea of the previous plane; an LCD's panel
    // RAM already holds it (and s_pushed mirrors it), so only validate.
    if (prev_canvas == nullptr) return ESP_ERR_INVALID_STATE;
    return partial("partial");
}

esp_err_t sticky_display_clear()
{
    if (s_canvas == nullptr) return ESP_ERR_INVALID_STATE;
    s_canvas->clear();
    return full("clear");
}

esp_err_t sticky_display_sleep()
{
    if (s_canvas == nullptr) return ESP_ERR_INVALID_STATE;
    if (!s_awake) return ESP_OK;
    s_awake = false;
    esp_err_t err = s_ops->brightness(0);
    const esp_err_t e = s_ops->sleep(true);
    if (e == ESP_OK) s_slept = true;
    return err != ESP_OK ? err : e;
}

esp_err_t lcd_canvas_set_brightness(uint8_t percent)
{
    if (percent > 100) percent = 100;
    s_brightness = percent;
    if (s_ops == nullptr || !s_awake) return ESP_OK;
    return s_ops->brightness(percent);
}

uint8_t lcd_canvas_brightness() { return s_brightness; }
