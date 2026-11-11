#pragma once
// Lock-free single-producer / single-consumer ring buffer.
//
// * Exactly one thread calls tryPush, exactly one other thread calls tryPop.
// * head_ (written by consumer) and tail_ (written by producer) live on separate
//   cache lines so the two cores don't ping-pong one line (false sharing).
// * Each side keeps a cached copy of the other side's index and only re-reads the
//   shared atomic when the cache says "full"/"empty". In steady state this means
//   most operations touch no cache line owned by the other core.
// * Memory ordering: the producer writes the slot, then publishes tail_ with
//   release; the consumer reads tail_ with acquire, then reads the slot.
//   Symmetrically for head_ (slot is free to reuse after consumer's release).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace ob {

inline constexpr std::size_t kCacheLine = 64;

template <class T, std::size_t Capacity>
class SpscRing {
    static_assert(Capacity >= 2 && (Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");
    static_assert(std::is_trivially_copyable_v<T>, "keep ring payloads trivially copyable");

public:
    bool tryPush(const T& v) noexcept {
        const std::uint64_t t = tail_.load(std::memory_order_relaxed);
        if (t - headCache_ == Capacity) {
            headCache_ = head_.load(std::memory_order_acquire);
            if (t - headCache_ == Capacity) return false;  // full
        }
        buf_[t & kMask] = v;
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    bool tryPop(T& out) noexcept {
        const std::uint64_t h = head_.load(std::memory_order_relaxed);
        if (h == tailCache_) {
            tailCache_ = tail_.load(std::memory_order_acquire);
            if (h == tailCache_) return false;  // empty
        }
        out = buf_[h & kMask];
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Approximate; only for monitoring.
    std::size_t sizeApprox() const noexcept {
        return static_cast<std::size_t>(tail_.load(std::memory_order_relaxed) -
                                        head_.load(std::memory_order_relaxed));
    }

private:
    static constexpr std::uint64_t kMask = Capacity - 1;

    // Producer-owned line.
    alignas(kCacheLine) std::atomic<std::uint64_t> tail_{0};
    std::uint64_t headCache_ = 0;
    // Consumer-owned line.
    alignas(kCacheLine) std::atomic<std::uint64_t> head_{0};
    std::uint64_t tailCache_ = 0;
    // Payload, starting on its own line.
    alignas(kCacheLine) T buf_[Capacity];
};

}  // namespace ob
