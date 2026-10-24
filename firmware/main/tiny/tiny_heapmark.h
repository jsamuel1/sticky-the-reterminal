// tiny_heapmark — where did the internal RAM go during boot?
//
// tiny_upload refuses a TLS upload below 60 KB of free INTERNAL|8BIT heap, and
// on the colour boards that rail is shared by the LCD DMA strips, the I2S
// rings, the SDMMC host, BLE, Wi-Fi and every task stack. A single status
// number cannot say which one ate it. Boot calls tiny_heapmark("<stage>")
// after each subsystem comes up; each mark logs, and `status` reports them
// all (`internal_boot`), so the cost of each stage is the difference between
// two neighbouring marks, read off the glass over the relay.
//
// Marks are stored in call order. A repeated name overwrites its earlier
// value in place (a re-run stage keeps one row). At most TINY_HEAPMARK_MAX
// marks are kept; later new names are dropped and counted.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TINY_HEAPMARK_MAX 16
#define TINY_HEAPMARK_NAME_MAX 11  // chars, excluding the terminator

typedef struct {
    char name[TINY_HEAPMARK_NAME_MAX + 1];
    uint32_t free_b;     // heap_caps_get_free_size(INTERNAL|8BIT)
    uint32_t largest_b;  // heap_caps_get_largest_free_block(INTERNAL|8BIT)
} tiny_heapmark_t;

// Record (and log) the internal heap now, under `name`.
void tiny_heapmark(const char *name);
// Same, with the two numbers supplied (the host test's entry point).
void tiny_heapmark_put(const char *name, uint32_t free_b, uint32_t largest_b);
int tiny_heapmark_count(void);
bool tiny_heapmark_get(int i, tiny_heapmark_t *out);  // false when out of range
int tiny_heapmark_dropped(void);                 // new names refused when full

#ifdef __cplusplus
}
#endif
