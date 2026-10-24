// tiny_screenshot — what is ON the panel, as a PNG, from the device itself.
// Written uncompressed (STORED deflate blocks, tiny/tiny_png.h): no
// compression library, fully deterministic. Two shapes, picked by the canvas:
//   e-ink (no accent plane): 2-bit grayscale, the 4 levels exactly — 800x480
//     -> ~96.6 KB, byte-identical to every screenshot before grammar v16.
//   colour board (accent plane): 8-bit indexed + a 64-entry PLTE holding the
//     glass's own theme colours (lcd_palette::color), so the PNG shows the
//     colours the panel shows — 320x480 -> ~154 KB.
// Base64 of either stays far under the 6 MB media cap. crc32 comes from ROM
// (esp_rom_crc32_le(0,…) == zlib crc32). Upload goes through the same
// tiny_upload_media path the mic uses, so the reply is a hosted URL the
// owner's surfaces can already show.
#include "tiny/tiny_screenshot.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "canvas.h"
#include "lcd_palette.h"
#include "tiny/board_caps.h"
#include "tiny/tiny_png.h"
#include "sticky_display.h"
#include "tiny/tiny_display.h"
#include "tiny/tiny_upload.h"

static const char *TAG = "tiny_shot";

static uint32_t rom_crc32(uint32_t crc, const uint8_t *buf, size_t len) {
    return esp_rom_crc32_le(crc, buf, len);
}

// Stage autopsy (P1 hunt 2026-08-26): the verb-level black box convicted
// `screenshot` 3/3 deterministic but not the stage. These markers ride the
// same RTC-noinit crumb; a panic now reads screenshot@snapshot/@png/@upload.
extern "C" void tiny_node_crumb_step(const char *step);

static void *big_alloc(size_t n) {
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

extern "C" esp_err_t tiny_screenshot_png(uint8_t **out, size_t *out_len) {
    if (!out || !out_len) return ESP_ERR_INVALID_ARG;
    // Panel-native size, from the canvas the snapshot copies.
    const Canvas *cv = sticky_display_canvas();
    if (!cv) return ESP_ERR_INVALID_STATE;
    const int W = cv->physical_width(), H = cv->physical_height();
    uint8_t *ix = NULL; size_t ix_len = 0;
    bool colour = false;
    tiny_node_crumb_step("snapshot");
    esp_err_t err = tiny_display_snapshot_index8(&ix, &ix_len, &colour);
    if (err != ESP_OK) return err;
    if (ix_len != (size_t)W * H) { free(ix); return ESP_ERR_INVALID_SIZE; }
    tiny_node_crumb_step("png");

    // Raw scanlines, filter byte 0 first. Gray: 4 pixels/byte, 2 bits each,
    // MSB-first (PNG bit order) — the index IS the level (accent 0).
    // Colour: one palette index per byte.
    const int row_bytes = colour ? W : W * 2 / 8;
    const size_t raw_len = (size_t)H * (row_bytes + 1);
    uint8_t *raw = (uint8_t *)big_alloc(raw_len);
    if (!raw) { free(ix); return ESP_ERR_NO_MEM; }
    size_t ri = 0;
    for (int y = 0; y < H; ++y) {
        raw[ri++] = 0;  // filter: None
        const uint8_t *px = ix + (size_t)y * W;
        if (colour) {
            memcpy(raw + ri, px, W);
            ri += W;
        } else {
            for (int xb = 0; xb < row_bytes; ++xb, px += 4)
                raw[ri++] = (uint8_t)(((px[0] & 3) << 6) | ((px[1] & 3) << 4) |
                                      ((px[2] & 3) << 2) | (px[3] & 3));
        }
    }
    free(ix);

    uint8_t plte[3 * lcd_palette::kPaletteSize];
    if (colour) {
        const lcd_palette::Theme t = tiny_board_caps()->dark_theme
                                         ? lcd_palette::Theme::Dark
                                         : lcd_palette::Theme::Light;
        for (int i = 0; i < lcd_palette::kPaletteSize; ++i) {
            const lcd_palette::Rgb c =
                lcd_palette::color(t, (uint8_t)(i >> 2), (uint8_t)(i & 3));
            plte[3 * i] = c.r; plte[3 * i + 1] = c.g; plte[3 * i + 2] = c.b;
        }
    }
    const int entries = colour ? lcd_palette::kPaletteSize : 0;
    const size_t cap = tiny_png::capacity(raw_len, entries);
    uint8_t *png = (uint8_t *)big_alloc(cap);
    if (!png) { free(raw); return ESP_ERR_NO_MEM; }
    const size_t n = colour
        ? tiny_png::encode(png, W, H, 8, 3, plte, entries, raw, raw_len, rom_crc32)
        : tiny_png::encode(png, W, H, 2, 0, NULL, 0, raw, raw_len, rom_crc32);
    free(raw);

    *out = png;
    *out_len = n;
    ESP_LOGI(TAG, "png ready: %u bytes (%s)", (unsigned)n,
             colour ? "indexed colour" : "2-bit gray");
    return ESP_OK;
}

extern "C" esp_err_t tiny_screenshot_upload(char *url, size_t url_cap) {
    uint8_t *png = NULL; size_t len = 0;
    esp_err_t err = tiny_screenshot_png(&png, &len);
    if (err != ESP_OK) return err;
    tiny_node_crumb_step("upload");
    err = tiny_upload_media(png, len, "image/png", url, url_cap);
    free(png);
    return err;
}
