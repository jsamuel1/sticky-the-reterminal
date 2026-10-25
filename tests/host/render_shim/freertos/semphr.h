#pragma once

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

// Test hook: called once when a thread starts an outer recursive-lock attempt.
// render_cases.cpp uses it to prove concurrent display operations wait before
// touching cache or rotation state.
void render_test_render_mutex_attempt();

// Recursive-lock levels the CALLING thread holds (tiny_display's render lock
// is the only recursive mutex). A seam reads it to prove, deterministically
// rather than by timing, that it runs inside render ownership.
inline thread_local unsigned render_test_recursive_held = 0;

struct RenderTestRecursiveMutex {
    std::mutex state;
    std::condition_variable ready;
    std::thread::id owner;
    unsigned depth = 0;
    uint64_t next_ticket = 0;
    uint64_t serving = 0;
};

typedef void *SemaphoreHandle_t;

static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return new std::mutex;
}

static inline int xSemaphoreTake(SemaphoreHandle_t handle, unsigned long) {
    static_cast<std::mutex *>(handle)->lock();
    return 1;
}

static inline int xSemaphoreGive(SemaphoreHandle_t handle) {
    static_cast<std::mutex *>(handle)->unlock();
    return 1;
}

static inline SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) {
    return new RenderTestRecursiveMutex;
}

static inline int xSemaphoreTakeRecursive(SemaphoreHandle_t handle,
                                          unsigned long) {
    auto *mutex = static_cast<RenderTestRecursiveMutex *>(handle);
    const std::thread::id self = std::this_thread::get_id();
    std::unique_lock<std::mutex> lock(mutex->state);
    if (mutex->owner == self) {
        ++mutex->depth;
        ++render_test_recursive_held;
        return 1;
    }
    const uint64_t ticket = mutex->next_ticket++;
    render_test_render_mutex_attempt();
    mutex->ready.wait(lock, [&] {
        return mutex->depth == 0 && ticket == mutex->serving;
    });
    mutex->owner = self;
    mutex->depth = 1;
    ++mutex->serving;
    ++render_test_recursive_held;
    return 1;
}

static inline int xSemaphoreGiveRecursive(SemaphoreHandle_t handle) {
    auto *mutex = static_cast<RenderTestRecursiveMutex *>(handle);
    std::lock_guard<std::mutex> lock(mutex->state);
    if (mutex->owner != std::this_thread::get_id() || mutex->depth == 0)
        return 0;
    --render_test_recursive_held;
    if (--mutex->depth == 0) {
        mutex->owner = std::thread::id{};
        mutex->ready.notify_all();
    }
    return 1;
}
