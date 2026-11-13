#pragma once

#include <chrono>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <time.h>

struct RequestWaits {
    uint64_t lock_us = 0, io_us = 0, top_us = 0;
    const char* top_site = "none";
    std::string top_coll;
};
inline thread_local RequestWaits request_waits;

inline uint64_t request_time_us() {
    return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline uint64_t request_thread_cpu_us() {
    timespec cpu_time{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_time);
    return cpu_time.tv_sec * 1000000ULL + cpu_time.tv_nsec / 1000;
}
template<class Mutex>
std::shared_lock<Mutex> timed_shared_lock(Mutex& mutex, const char* site, const std::string& collection_name) {
    std::shared_lock<Mutex> lock(mutex, std::try_to_lock);
    if(!lock.owns_lock()) {
        const auto wait_start = request_time_us();
        lock.lock();
        const auto waited_us = request_time_us() - wait_start;
        request_waits.lock_us += waited_us;
        if(waited_us > request_waits.top_us) {
            request_waits.top_us = waited_us;
            request_waits.top_site = site;
            request_waits.top_coll = collection_name;
        }
    }
    return lock;
}
