#pragma once
// Fixed-capacity object pool. All storage is allocated once in the
// constructor; alloc()/release() are O(1) and never touch the heap.
// Objects are addressed by 32-bit index rather than pointer: half the size
// of a pointer, which keeps Order and Level structs small and cache-dense.

#include <cstdint>
#include <vector>

namespace ob {

inline constexpr std::uint32_t kNil = 0xFFFFFFFFu;

template <class T>
class ObjectPool {
public:
    explicit ObjectPool(std::uint32_t capacity) : slots_(capacity) {
        free_.reserve(capacity);
        // Push in reverse so the first alloc() returns index 0 (nicer for debugging,
        // and gives sequential, prefetch-friendly addresses on a fresh pool).
        for (std::uint32_t i = capacity; i-- > 0;) free_.push_back(i);
    }

    // Returns kNil when exhausted.
    std::uint32_t alloc() noexcept {
        if (free_.empty()) return kNil;
        std::uint32_t idx = free_.back();
        free_.pop_back();
        return idx;
    }

    void release(std::uint32_t idx) noexcept { free_.push_back(idx); }  // capacity reserved: no realloc

    T&       operator[](std::uint32_t i) noexcept { return slots_[i]; }
    const T& operator[](std::uint32_t i) const noexcept { return slots_[i]; }

    std::uint32_t capacity() const noexcept { return static_cast<std::uint32_t>(slots_.size()); }
    std::uint32_t inUse() const noexcept { return capacity() - static_cast<std::uint32_t>(free_.size()); }

private:
    std::vector<T>             slots_;
    std::vector<std::uint32_t> free_;
};

}  // namespace ob
