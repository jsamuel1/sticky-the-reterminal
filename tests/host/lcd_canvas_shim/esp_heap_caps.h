#pragma once

#include <cstdlib>

#define MALLOC_CAP_SPIRAM 1u
#define MALLOC_CAP_8BIT 2u
#define MALLOC_CAP_DMA 4u
#define MALLOC_CAP_INTERNAL 8u

static inline void *heap_caps_malloc(size_t n, unsigned) { return std::malloc(n); }
static inline void heap_caps_free(void *p) { std::free(p); }
