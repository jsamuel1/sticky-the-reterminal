// ask reservation — shared atomic slot for direct and queued asks/dictation.
#pragma once
#include <atomic>

namespace tiny_ask_reservation {
inline std::atomic<bool> &slot() {
    static std::atomic<bool> value{false};
    return value;
}
inline bool reserve() {
    bool expected = false;
    return slot().compare_exchange_strong(expected, true);
}
static inline void release() { slot().store(false); }
static inline bool held() { return slot().load(); }
}  // namespace tiny_ask_reservation
