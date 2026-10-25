#pragma once

#include <cstdint>

static inline int64_t esp_timer_get_time() {
    static int64_t now = 0;
    return ++now;
}
