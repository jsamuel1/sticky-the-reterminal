#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
// Failure injection: render_cases.cpp arms a byte size; every allocation of
// exactly that size (PSRAM AND the internal-RAM fallback, which the renderer
// reaches through plain malloc) returns NULL until disarmed. Only
// tiny_display.cpp includes this header, so the macro reaches nothing else.
void *render_test_malloc(size_t n);
static inline void *heap_caps_malloc(size_t n, unsigned) {
    return render_test_malloc(n);
}
static inline void heap_caps_free(void *p) { free(p); }
#define malloc(n) render_test_malloc(n)
