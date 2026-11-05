#pragma once
// Open-addressing hash map: uint64 key -> uint32 value, linear probing,
// backward-shift deletion (no tombstones, so probe lengths don't degrade
// over a long session of add/cancel churn). Fixed capacity: sized once,
// never rehashes, never allocates after construction.
//
// Why not std::unordered_map: it allocates a node per insert and chases a
// pointer per lookup. Here a lookup is usually one cache line.

#include <cassert>
#include <cstdint>
#include <vector>

#include "orderbook/types.hpp"

namespace ob {

class FlatHashMap {
public:
    // maxEntries: the most keys that will ever be live at once.
    // Capacity is the next power of two >= 2 * maxEntries (load factor <= 0.5).
    explicit FlatHashMap(std::uint32_t maxEntries) {
        std::uint64_t cap = 16;
        while (cap < 2ull * maxEntries) cap <<= 1;
        slots_.assign(cap, Slot{kInvalidOrderId, 0});
        mask_ = cap - 1;
    }

    bool insert(std::uint64_t key, std::uint32_t value) noexcept {
        assert(key != kInvalidOrderId);
        std::uint64_t i = hash(key) & mask_;
        while (slots_[i].key != kInvalidOrderId) {
            if (slots_[i].key == key) return false;
            i = (i + 1) & mask_;
        }
        slots_[i] = Slot{key, value};
        ++size_;
        return true;
    }

    // Returns pointer to value or nullptr.
    const std::uint32_t* find(std::uint64_t key) const noexcept {
        std::uint64_t i = hash(key) & mask_;
        while (slots_[i].key != kInvalidOrderId) {
            if (slots_[i].key == key) return &slots_[i].value;
            i = (i + 1) & mask_;
        }
        return nullptr;
    }

    bool erase(std::uint64_t key) noexcept {
        std::uint64_t i = hash(key) & mask_;
        while (slots_[i].key != kInvalidOrderId) {
            if (slots_[i].key == key) {
                eraseAt(i);
                --size_;
                return true;
            }
            i = (i + 1) & mask_;
        }
        return false;
    }

    std::uint64_t size() const noexcept { return size_; }
    std::uint64_t capacity() const noexcept { return mask_ + 1; }

private:
    struct Slot {
        std::uint64_t key;
        std::uint32_t value;
    };

    // splitmix64 finaliser: sequential order IDs would otherwise cluster.
    static std::uint64_t hash(std::uint64_t x) noexcept {
        x ^= x >> 30;
        x *= 0xbf58476d1ce4e5b9ull;
        x ^= x >> 27;
        x *= 0x94d049bb133111ebull;
        x ^= x >> 31;
        return x;
    }

    // Backward-shift deletion: walk forward from the hole; any entry whose
    // ideal slot is NOT cyclically in (hole, j] can be moved back into the hole.
    void eraseAt(std::uint64_t hole) noexcept {
        std::uint64_t j = hole;
        for (;;) {
            j = (j + 1) & mask_;
            if (slots_[j].key == kInvalidOrderId) break;
            std::uint64_t ideal = hash(slots_[j].key) & mask_;
            bool idealInRange = (hole <= j) ? (hole < ideal && ideal <= j)
                                            : (hole < ideal || ideal <= j);
            if (idealInRange) continue;
            slots_[hole] = slots_[j];
            hole = j;
        }
        slots_[hole].key = kInvalidOrderId;
    }

    std::vector<Slot> slots_;
    std::uint64_t     mask_ = 0;
    std::uint64_t     size_ = 0;
};

}  // namespace ob
