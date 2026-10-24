// tiny_admit — the one admission gate between WORK and SLEEP.
//
// Deep sleep drops every rail, so it must never start while something is
// using the device: a relay verb, an ask (relay, AI/BOOT button, typed,
// queued drain) recording the mic or holding a TLS socket or painting its
// answer, a messages fetch. Before this gate the sleep paths checked a
// dispatch-only flag (the node task's black-box crumb) from other tasks with
// no synchronisation, so a PWR-key long press could power down in the middle
// of a physical ask.
//
// One atomic word: bit 31 = sleep claimed, bits 0..30 = work in flight.
//   * work_enter() refuses once sleep is claimed, else counts one in.
//     Nested work (dispatch -> ask, send -> open) just counts twice.
//   * sleep_claim() succeeds only from "no work, not claimed"; after that no
//     work can enter, so whatever the sleeper checks next (a stream playing,
//     the charger) cannot change under it. A sleeper that decides not to
//     sleep calls sleep_release().
//   * work_to_sleep() is the `sleep` verb's door: it runs as work, so it
//     claims only when it is the ONE piece of work in flight, in one step.
//
// Header-only, std::atomic only: tests/host/admit_host_test.sh races it on
// the host with real threads.
#pragma once

#include <atomic>
#include <stdint.h>

namespace tiny_admit {

constexpr uint32_t kSleep = 1u << 31;

inline std::atomic<uint32_t> &word()
{
    static std::atomic<uint32_t> w{0};
    return w;
}

// true = admitted; pair with work_exit(). false = sleep is claimed.
inline bool work_enter()
{
    uint32_t v = word().load();
    do {
        if (v & kSleep) return false;
    } while (!word().compare_exchange_weak(v, v + 1));
    return true;
}

inline void work_exit() { word().fetch_sub(1); }

// true = this caller now owns sleep; nothing new is admitted until release.
inline bool sleep_claim()
{
    uint32_t expect = 0;
    return word().compare_exchange_strong(expect, kSleep);
}

// From inside work: claim only if this caller is the only work in flight.
// On success the caller's own work count is consumed by the claim.
inline bool work_to_sleep()
{
    uint32_t expect = 1;
    return word().compare_exchange_strong(expect, kSleep);
}

inline void sleep_release() { word().fetch_and(~kSleep); }

inline bool sleep_claimed() { return (word().load() & kSleep) != 0; }
inline uint32_t work_in_flight() { return word().load() & ~kSleep; }

// RAII work slot: `Work w; if (!w) refuse;`
struct Work {
    bool ok;
    Work() : ok(work_enter()) {}
    ~Work() { if (ok) work_exit(); }
    Work(const Work &) = delete;
    Work &operator=(const Work &) = delete;
    explicit operator bool() const { return ok; }
};

}  // namespace tiny_admit
