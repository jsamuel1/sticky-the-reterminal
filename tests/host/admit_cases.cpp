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

    // Queue handoff: producer reserves before publishing; consumer adopts the
    // existing count without incrementing, so sleep stays excluded throughout.
    reset();
    CHECK("queue producer reserves", tiny_admit::work_enter());
    CHECK("sleep refused while work is queued", !tiny_admit::sleep_claim());
    {
        tiny_admit::Work queued(tiny_admit::AdoptReserved::Yes);
        CHECK("queue consumer adopts one count", tiny_admit::work_in_flight() == 1);
    }
    CHECK("queue consumer releases reservation", tiny_admit::word().load() == 0);

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

// Queue consumer shared by the queue tests. It exits only once EVERY producer
// has left its loop AND the mailbox is empty. `stop` alone is not a fence: a
// producer can read stop=false, pause before work_enter(), then reserve and
// publish after the consumer saw stop=true with an empty mailbox. That item's
// reservation would be stranded (the firmware consumer never exits, so this
// is a teardown fence for the test, not a product property).
static void run_consumer(std::atomic<int> &mailbox,
                         const std::atomic<int> &producers_done,
                         int producers, const std::atomic<bool> &stop,
                         std::atomic<int> &inside,
                         std::atomic<int> &violations,
                         std::atomic<int> &consumed,
                         std::atomic<bool> *empty_before_done = nullptr) {
    // `stop` is deliberately not the completion predicate. Keeping it in this
    // helper lets queue_late_producer pin the exact old predicate without a
    // scheduler delay: its producer reserves, stop becomes true, then the
    // consumer must remain alive solely because that producer is not done.
    (void)stop;
    while (producers_done.load() < producers || mailbox.load()) {
        int full = 1;
        if (!mailbox.compare_exchange_strong(full, 0)) {
            if (empty_before_done && producers_done.load() < producers)
                empty_before_done->store(true);
            continue;
        }
        tiny_admit::Work w(tiny_admit::AdoptReserved::Yes);
        inside.fetch_add(1);
        if (tiny_admit::sleep_claimed()) violations.fetch_add(1);
        consumed.fetch_add(1);
        inside.fetch_sub(1);
    }
}

// Deterministic teardown case: producer first reserves a gate slot, then waits.
// With stop=true and an empty mailbox, the consumer must stay alive because
// producer completion — not stop — is its fence. Once it has observed that
// exact state, publication is released and must be drained. Restoring the old
// `!stop || mailbox` condition exits before publication and fails both checks,
// independent of thread scheduling.
static void queue_late_producer() {
    reset();
    std::atomic<int> mailbox{0}, inside{0}, violations{0}, consumed{0};
    std::atomic<int> producers_done{0};
    std::atomic<bool> stop{false}, reserved{false}, publish{false};
    std::atomic<bool> empty_before_done{false}, consumer_done{false};
    std::thread producer([&] {
        if (tiny_admit::work_enter()) {
            reserved.store(true);
            while (!publish.load()) std::this_thread::yield();
            int empty = 0;
            if (!mailbox.compare_exchange_strong(empty, 1))
                tiny_admit::work_exit();
        }
        producers_done.fetch_add(1);
    });
    while (!reserved.load()) std::this_thread::yield();
    stop.store(true);
    std::thread consumer([&] {
        run_consumer(mailbox, producers_done, 1, stop, inside, violations,
                     consumed, &empty_before_done);
        consumer_done.store(true);
    });
    while (!empty_before_done.load() && !consumer_done.load())
        std::this_thread::yield();
    const bool consumer_waited = empty_before_done.load() && !consumer_done.load();
    publish.store(true);
    producer.join();
    consumer.join();
    CHECK("queue late producer: consumer waited for unfinished producer",
          consumer_waited);
    CHECK("queue late producer: item published after stop is consumed",
          consumed.load() == 1 && mailbox.load() == 0);
    CHECK("queue late producer: gate idle at end",
          tiny_admit::word().load() == 0);
}

// Queue handoff race: producers reserve, then publish into a depth-1
// mailbox (the firmware queues are depth 1..4, no wait); on a full mailbox
// they cancel. A consumer adopts the reservation. Sleepers claim and must
// then see the mailbox EMPTY and nobody inside: an accepted item is never
// stranded behind a claimed sleep. Before the fix (check sleep_claimed(),
// then publish unreserved) this property is exactly what failed.
static void queue_race() {
    reset();
    std::atomic<int> mailbox{0};  // 0 empty, 1 holds an item
    std::atomic<int> inside{0};
    std::atomic<bool> stop{false};
    std::atomic<int> violations{0}, claims{0}, consumed{0}, cancelled{0};
    std::atomic<int> producers_done{0};
    constexpr int kProducers = 3;
    std::vector<std::thread> ts;
    for (int i = 0; i < kProducers; ++i)
        ts.emplace_back([&] {
            while (!stop.load()) {
                if (!tiny_admit::work_enter()) continue;
                int empty = 0;
                if (!mailbox.compare_exchange_strong(empty, 1)) {
                    tiny_admit::work_exit();  // queue full: cancel
                    cancelled.fetch_add(1);
                }
            }
            producers_done.fetch_add(1);
        });
    ts.emplace_back([&] {
        run_consumer(mailbox, producers_done, kProducers, stop, inside,
                     violations, consumed);
    });
    for (int i = 0; i < 2; ++i)
        ts.emplace_back([&] {
            while (!stop.load()) {
                if (!tiny_admit::sleep_claim()) continue;
                claims.fetch_add(1);
                if (mailbox.load() != 0 || inside.load() != 0)
                    violations.fetch_add(1);
                std::this_thread::yield();
                if (mailbox.load() != 0 || inside.load() != 0)
                    violations.fetch_add(1);
                tiny_admit::sleep_release();
            }
        });
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    stop.store(true);
    for (auto &t : ts) t.join();
    printf("     queue race: %d consumed, %d cancelled, %d claims\n",
           consumed.load(), cancelled.load(), claims.load());
    CHECK("queue race: all sides made progress",
          consumed.load() > 0 && claims.load() > 0);
    CHECK("queue race: claimed sleep never saw a queued or running item",
          violations.load() == 0);
    CHECK("queue race: gate idle at end", tiny_admit::word().load() == 0);
}

int main() {
    sequential();
    race();
    queue_late_producer();
    queue_race();
    if (fails) { printf("%d FAILED\n", fails); return 1; }
    return 0;
}
