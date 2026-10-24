#pragma once

// lcd_canvas — the sticky_display_* HAL for every colour LCD/AMOLED board.
//
// The UI keeps drawing into the same 2-bit Canvas the e-ink board uses; this
// module owns that canvas, sized to the PANEL's physical pixels, and turns a
// refresh into a push: canvas rows -> RGB565 strips in two DMA-capable bounce
// buffers (convert one while the other is on the wire) -> the panel.
//
// Refresh mapping (an LCD has no waveforms):
//   refresh / refresh_monochrome / clear  -> full frame
//   refresh_partial / partial_baseline    -> only the rows that differ from
//                                            the last frame pushed (a copy is
//                                            kept); baseline is irrelevant
//                                            because the panel RAM, not the
//                                            caller, holds the old picture
//   sleep                                 -> backlight off + panel sleep;
//                                            the next refresh wakes it
//
// Canvas size decision (P2, 2026-10-24): the canvas IS the panel, e.g. 320x480
// on the FNK0104S. No scaling or letterboxing: tiny_display lays out against
// Canvas::width()/height() already, rotation still lives in Canvas, and the
// screenshot, tap regions and touch all share the panel's own coordinates.
//
// A board adds exactly one panel file that defines lcd_panel_ops():
//   FNK0104S  lcd_panel_st7796.cpp  ST7796S 320x480 SPI LCD
//   AMOLED18  lcd_panel_co5300.cpp  CO5300 368x448 QSPI AMOLED (P2b)

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

struct LcdPanelOps {
    const char *name;       // for logs, e.g. "ST7796S"
    uint16_t width;         // physical pixels in the panel's native orientation
    uint16_t height;        //   (width must be a multiple of 4)
    // Every draw()'s y0 and y1 are multiples of this (1 = any row). The
    // CO5300 takes windows in 2-pixel units; a partial refresh widens its
    // dirty band outward to meet it. height must be a multiple too.
    uint8_t row_align;
    // Bring up the bus and panel, backlight OFF. `max_transfer_bytes` = one
    // strip, the largest single draw() the canvas will issue.
    esp_err_t (*init)(size_t max_transfer_bytes);
    // Queue big-endian RGB565 for rows [y0, y1), full width. `px` must stay
    // untouched until the transfer completes: the panel signals each finished
    // draw through wait_done().
    esp_err_t (*draw)(int y0, int y1, const uint16_t *px);
    // Block until one queued draw() has finished (FIFO), or timeout.
    esp_err_t (*wait_done)(uint32_t timeout_ms);
    esp_err_t (*sleep)(bool sleep);
    // 0..100. 0 = dark. A panel without a dimmer may treat >0 as on.
    // (ST7796: LEDC backlight duty. CO5300: DCS 0x51, the AMOLED's own
    // emission level - there is no backlight.)
    esp_err_t (*brightness)(uint8_t percent);
};

// Defined by the board's panel file (lcd_panel_st7796.cpp, ...).
const LcdPanelOps *lcd_panel_ops();

// Backlight level used whenever the panel is awake (default 80). Applied now
// if the panel is lit; otherwise remembered for the next refresh.
esp_err_t lcd_canvas_set_brightness(uint8_t percent);
uint8_t lcd_canvas_brightness();
