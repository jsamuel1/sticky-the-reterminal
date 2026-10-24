// admit host test: races the SHIPPED firmware/main/tiny/tiny_admit.h with
// real threads. The property: a sleeper never holds the claim while any
// worker is inside its admitted section, and once claimed no worker enters.
#include "tiny/tiny_admit.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int fails = 0;
#define CHECK(name, cond) do { \
    if (cond) printf("ok   %s\n", name); \
    else { printf("FAIL %s\n", name); fails++; } \
} while (0)

static void reset() { tiny_admit::word().store(0); }

static void sequential() {
    reset();
    {
        tiny_admit::Work w;
        CHECK("work admitted when idle", (bool)w);
        CHECK("sleep refused while work in flight", !tiny_admit::sleep_claim());
        {
            tiny_admit::Work nested;
            CHECK("nested work admitted", (bool)nested);
            CHECK("work_to_sleep refused with two in flight",
                  !tiny_admit::work_to_sleep());
        }
        CHECK("one in flight after nested exit", tiny_admit::work_in_flight() == 1);
    }
    CHECK("idle after work exit", tiny_admit::word().load() == 0);

    CHECK("sleep claimed when idle", tiny_admit::sleep_claim());
    CHECK("second sleeper refused", !tiny_admit::sleep_claim());
    {
        tiny_admit::Work w;
        CHECK("work refused once sleep claimed", !w);
    }
    CHECK("refused work leaves no count", tiny_admit::work_in_flight() == 0);
    tiny_admit::sleep_release();
    {
        tiny_admit::Work w;
        CHECK("work admitted after release", (bool)w);
    }

    // The `sleep` verb: it is work itself, and claims only as the sole work.
    reset();
    CHECK("verb enters", tiny_admit::work_enter());
    CHECK("verb claims as sole work", tiny_admit::work_to_sleep());
    CHECK("verb claim consumed its count",
          tiny_admit::sleep_claimed() && tiny_admit::work_in_flight() == 0);
    {
        tiny_admit::Work w;
        CHECK("work refused after verb claimed", !w);
    }
}

// Workers spin entering/leaving; sleepers claim, verify no worker is inside,
// release. `inside` is the ground truth the gate must protect.
static void race() {
    reset();
    std::atomic<int> inside{0};
    std::atomic<bool> stop{false};
    std::atomic<int> violations{0}, claims{0}, admits{0};
    std::vector<std::thread> ts;
    for (int i = 0; i < 4; ++i)
        ts.emplace_back([&] {
            while (!stop.load()) {
                tiny_admit::Work w;
                if (!w) continue;
                admits.fetch_add(1);
                inside.fetch_add(1);
                if (tiny_admit::sleep_claimed()) violations.fetch_add(1);
                inside.fetch_sub(1);
            }
        });
    for (int i = 0; i < 2; ++i)
        ts.emplace_back([&] {
            while (!stop.load()) {
                if (!tiny_admit::sleep_claim()) continue;
                claims.fetch_add(1);
                if (inside.load() != 0) violations.fetch_add(1);
                std::this_thread::yield();
                if (inside.load() != 0) violations.fetch_add(1);
                tiny_admit::sleep_release();
            }
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop.store(true);
    for (auto &t : ts) t.join();
    printf("     race: %d admits, %d claims\n", admits.load(), claims.load());
    CHECK("race: both sides made progress", admits.load() > 0 && claims.load() > 0);
    CHECK("race: sleep never overlapped work", violations.load() == 0);
    CHECK("race: gate idle at end", tiny_admit::word().load() == 0);
}

int main() {
    sequential();
    race();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    return 0;
}
