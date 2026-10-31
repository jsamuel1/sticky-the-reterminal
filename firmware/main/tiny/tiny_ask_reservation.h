// ask reservation — shared atomic slot for direct and queued asks/dictation.
#pragma once
#include <atomic>

// Every function is plain `inline` (external linkage), never `static inline`:
// the slot is a function-local static, and with internal linkage each
// translation unit (tiny_node.cpp, tiny_button.cpp, ...) would get its OWN
// slot. Two independent slots means no single-flight at all.
namespace tiny_ask_reservation {
inline std::atomic<bool> &slot() {
    static std::atomic<bool> value{false};
    return value;
}
inline bool reserve() {
    bool expected = false;
    return slot().compare_exchange_strong(expected, true);
}
inline void release() { slot().store(false); }
inline bool held() { return slot().load(); }
}  // namespace tiny_ask_reservation
