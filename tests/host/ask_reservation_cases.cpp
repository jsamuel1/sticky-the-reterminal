// ask reservation host test — races shipped primitive with real threads.
#include "tiny/tiny_ask_reservation.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int g_fail = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    ++g_fail; \
} } while (0)

// ask_reservation_other_tu.cpp: the same header, compiled separately.
bool other_tu_reserve();
void other_tu_release();
bool other_tu_held();

int main() {
    // One slot across translation units, the way tiny_button.cpp (producer)
    // and tiny_node.cpp (engine) share it. Per-TU slots would let both win.
    tiny_ask_reservation::release();
    CHECK(other_tu_reserve());
    CHECK(tiny_ask_reservation::held());
    CHECK(!tiny_ask_reservation::reserve());
    tiny_ask_reservation::release();
    CHECK(!other_tu_held());
    CHECK(tiny_ask_reservation::reserve());
    CHECK(!other_tu_reserve());
    other_tu_release();
    CHECK(!tiny_ask_reservation::held());
    // Test isolation only; production releases only from its current owner.
    tiny_ask_reservation::release();
    CHECK(tiny_ask_reservation::reserve());
    CHECK(!tiny_ask_reservation::reserve());
    tiny_ask_reservation::release();
    CHECK(tiny_ask_reservation::reserve());
    tiny_ask_reservation::release();

    for (int round = 0; round < 200; ++round) {
        // Prior round's one winner still owns the slot.
        tiny_ask_reservation::release();
        std::atomic<bool> go{false};
        std::atomic<int> winners{0};
        std::vector<std::thread> threads;
        for (int i = 0; i < 16; ++i) {
            threads.emplace_back([&] {
                while (!go.load(std::memory_order_acquire)) {}
                if (tiny_ask_reservation::reserve())
                    winners.fetch_add(1, std::memory_order_relaxed);
            });
        }
        go.store(true, std::memory_order_release);
        for (auto &t : threads) t.join();
        CHECK(winners.load() == 1);
        CHECK(!tiny_ask_reservation::reserve());
    }
    tiny_ask_reservation::release();
    CHECK(tiny_ask_reservation::reserve());
    tiny_ask_reservation::release();

    if (g_fail) {
        std::fprintf(stderr, "ask reservation host test: %d FAILED\n", g_fail);
        return 1;
    }
    std::puts("ask reservation host test: ALL GREEN");
    return 0;
}
