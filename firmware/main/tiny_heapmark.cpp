// tiny_heapmark — see tiny/tiny_heapmark.h.
#include "tiny/tiny_heapmark.h"

#include <string.h>

#ifndef TINY_HEAPMARK_HOST
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#endif

namespace {

tiny_heapmark_t s_marks[TINY_HEAPMARK_MAX];
int s_count = 0;
int s_dropped = 0;

#ifndef TINY_HEAPMARK_HOST
// Marks are written from app_main and read from the node task (`status`).
// A spinlock that is initialised at compile time has no first-use race, and
// every critical section below is a short table walk or a 20-byte copy.
portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
struct Lock {
    Lock() { portENTER_CRITICAL(&s_mux); }
    ~Lock() { portEXIT_CRITICAL(&s_mux); }
};
#else
struct Lock {
    Lock() {}  // user-provided: no unused-variable warning on the host
};
#endif

}  // namespace

extern "C" void tiny_heapmark_put(const char *name, uint32_t free_b,
                                  uint32_t largest_b) {
    if (!name || !name[0]) return;
    Lock lock;
    tiny_heapmark_t *m = nullptr;
    for (int i = 0; i < s_count; ++i)
        if (strncmp(s_marks[i].name, name, TINY_HEAPMARK_NAME_MAX) == 0) {
            m = &s_marks[i];
            break;
        }
    if (!m) {
        if (s_count >= TINY_HEAPMARK_MAX) {
            ++s_dropped;
            return;
        }
        m = &s_marks[s_count++];
        strncpy(m->name, name, TINY_HEAPMARK_NAME_MAX);
        m->name[TINY_HEAPMARK_NAME_MAX] = 0;
    }
    m->free_b = free_b;
    m->largest_b = largest_b;
}

#ifndef TINY_HEAPMARK_HOST
extern "C" void tiny_heapmark(const char *name) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t f = (uint32_t)heap_caps_get_free_size(caps);
    const uint32_t l = (uint32_t)heap_caps_get_largest_free_block(caps);
    tiny_heapmark_put(name, f, l);
    ESP_LOGI("heapmark", "%-11s internal free %6u B, largest block %6u B",
             name ? name : "?", (unsigned)f, (unsigned)l);
}
#endif

extern "C" int tiny_heapmark_count(void) {
    Lock lock;
    return s_count;
}

extern "C" bool tiny_heapmark_get(int i, tiny_heapmark_t *out) {
    Lock lock;
    if (!out || i < 0 || i >= s_count) return false;
    *out = s_marks[i];  // a copy: the writer may overwrite the slot later
    return true;
}

extern "C" int tiny_heapmark_dropped(void) {
    Lock lock;
    return s_dropped;
}
