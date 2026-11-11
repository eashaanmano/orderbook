#pragma once
// Small OS helpers: monotonic clock, thread pinning, spin-wait hint.

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace ob {

inline std::uint64_t nowNs() noexcept {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

inline void cpuRelax() noexcept {
#if defined(__x86_64__) || defined(_M_X64)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
}

// Pin the calling thread to one core. Returns false (and keeps running) if unsupported.
inline bool pinThisThread(int core) {
    if (core < 0) return false;
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    const int rc = pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    if (rc != 0) std::fprintf(stderr, "warning: could not pin thread to core %d (rc=%d)\n", core, rc);
    return rc == 0;
#else
    (void)core;
    return false;
#endif
}

// Prevent the optimiser from discarding a computed value in benchmarks.
template <class T>
inline void doNotOptimize(T const& v) {
    asm volatile("" : : "r,m"(v) : "memory");
}

inline std::vector<int> parseIntList(const std::string& s) {
    std::vector<int> out;
    std::size_t      pos = 0;
    while (pos < s.size()) {
        std::size_t next = s.find(',', pos);
        if (next == std::string::npos) next = s.size();
        out.push_back(std::stoi(s.substr(pos, next - pos)));
        pos = next + 1;
    }
    return out;
}

}  // namespace ob
