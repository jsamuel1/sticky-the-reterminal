// render_shim — the host stand-ins tiny_display.cpp needs to compile and draw
// on a desktop. Force-included (-include) ahead of the shipped source, so the
// renderer under test is the device's own file, unmodified.
//
// Only the drawing surface is real (canvas.cpp + font.cpp + qrcodegen +
// cJSON, the same versions the device links). Everything with a side effect
// (panel, network, NVS, buzzer, tasks) is a no-op that answers "nothing here",
// which is what the honest-void paths in the renderer already expect.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// glibc < 2.38 has no strlcpy/strlcat; the device's newlib does.
#ifndef RENDER_SHIM_HAVE_STRLCPY
static inline size_t strlcpy(char *d, const char *s, size_t n) {
    const size_t l = strlen(s);
    if (n) { const size_t c = l < n - 1 ? l : n - 1; memcpy(d, s, c); d[c] = 0; }
    return l;
}
#endif

#define RTC_DATA_ATTR
#define EXT_RAM_BSS_ATTR
