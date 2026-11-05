#pragma once
// One bit per price level: set = level has resting orders.
// When the best level empties, the next best is found by scanning 64 levels
// per instruction (count-leading/trailing-zeros) instead of one level at a time.

#include <cstdint>
#include <vector>

namespace ob {

class LevelBitmap {
public:
    explicit LevelBitmap(std::uint32_t numBits) : n_(numBits), words_((numBits + 63) / 64, 0) {}

    void set(std::uint32_t i) noexcept { words_[i >> 6] |= (1ull << (i & 63)); }
    void clear(std::uint32_t i) noexcept { words_[i >> 6] &= ~(1ull << (i & 63)); }
    bool test(std::uint32_t i) const noexcept { return (words_[i >> 6] >> (i & 63)) & 1ull; }

    // Highest set index <= i, or -1.
    std::int64_t highestAtOrBelow(std::int64_t i) const noexcept {
        if (i < 0) return -1;
        if (i >= static_cast<std::int64_t>(n_)) i = n_ - 1;
        std::int64_t w   = i >> 6;
        unsigned     bit = static_cast<unsigned>(i & 63);
        std::uint64_t m  = words_[w] & (bit == 63 ? ~0ull : ((1ull << (bit + 1)) - 1));
        while (true) {
            if (m) return w * 64 + 63 - __builtin_clzll(m);
            if (--w < 0) return -1;
            m = words_[w];
        }
    }

    // Lowest set index >= i, or -1.
    std::int64_t lowestAtOrAbove(std::int64_t i) const noexcept {
        if (i < 0) i = 0;
        if (i >= static_cast<std::int64_t>(n_)) return -1;
        std::int64_t  w = i >> 6;
        std::uint64_t m = words_[w] & (~0ull << (i & 63));
        const std::int64_t nw = static_cast<std::int64_t>(words_.size());
        while (true) {
            if (m) return w * 64 + __builtin_ctzll(m);  // bits >= n_ are never set
            if (++w >= nw) return -1;
            m = words_[w];
        }
    }

private:
    std::uint32_t              n_;
    std::vector<std::uint64_t> words_;
};

}  // namespace ob
