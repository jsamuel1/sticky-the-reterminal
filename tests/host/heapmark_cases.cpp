// heapmark host test — the SHIPPED firmware/main/tiny_heapmark.cpp
// (TINY_HEAPMARK_HOST: no IDF heap, no mutex): call order kept, a repeated
// stage name overwrites in place, long names clip, overflow is counted.
#include <cassert>
#include <cstdio>
#include <cstring>

#include "tiny/tiny_heapmark.h"

int main() {
    tiny_heapmark_t m;
    assert(tiny_heapmark_count() == 0);
    assert(!tiny_heapmark_get(0, &m));

    tiny_heapmark_put("start", 200000, 150000);
    tiny_heapmark_put("display", 178000, 120000);
    tiny_heapmark_put("start", 199000, 149000);  // same stage again
    assert(tiny_heapmark_count() == 2);
    assert(tiny_heapmark_get(0, &m) && !strcmp(m.name, "start") &&
           m.free_b == 199000 && m.largest_b == 149000);
    assert(tiny_heapmark_get(1, &m) && !strcmp(m.name, "display"));
    assert(!tiny_heapmark_get(-1, &m) && !tiny_heapmark_get(2, &m));

    tiny_heapmark_put("", 1, 1);       // ignored
    tiny_heapmark_put(nullptr, 1, 1);  // ignored
    assert(tiny_heapmark_count() == 2);

    tiny_heapmark_put("averyverylongstagename", 5, 4);
    assert(tiny_heapmark_get(2, &m));
    assert(strlen(m.name) == TINY_HEAPMARK_NAME_MAX);
    assert(!strncmp(m.name, "averyverylongstagename", TINY_HEAPMARK_NAME_MAX));
    // The clipped name is the key: the same long name updates that row.
    tiny_heapmark_put("averyverylongstagename", 6, 4);
    assert(tiny_heapmark_count() == 3);
    assert(tiny_heapmark_get(2, &m) && m.free_b == 6);

    for (int i = tiny_heapmark_count(); i < TINY_HEAPMARK_MAX; ++i) {
        char n[16];
        snprintf(n, sizeof n, "s%d", i);
        tiny_heapmark_put(n, i, i);
    }
    assert(tiny_heapmark_count() == TINY_HEAPMARK_MAX);
    assert(tiny_heapmark_dropped() == 0);
    tiny_heapmark_put("extra", 1, 1);
    assert(tiny_heapmark_count() == TINY_HEAPMARK_MAX);
    assert(tiny_heapmark_dropped() == 1);
    tiny_heapmark_put("start", 7, 7);  // an existing name still updates when full
    assert(tiny_heapmark_get(0, &m) && m.free_b == 7);
    puts("heapmark cases: ok");
    return 0;
}
