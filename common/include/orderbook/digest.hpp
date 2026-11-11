#pragma once
// Order-sensitive FNV-1a digest of a trade stream. Two runs produced the same
// trades in the same order iff (with overwhelming probability) digests match.
// Used to show the threaded pipeline is exactly as deterministic as single-threaded replay.

#include <cstdint>

#include "orderbook/types.hpp"

namespace ob {

class TradeDigest {
public:
    void add(const Trade& t) noexcept {
        mix(t.taker);
        mix(t.maker);
        mix(static_cast<std::uint64_t>(t.price));
        mix(static_cast<std::uint64_t>(t.qty));
        mix(static_cast<std::uint64_t>(t.takerSide));
        ++count_;
    }
    std::uint64_t value() const noexcept { return h_; }
    std::uint64_t count() const noexcept { return count_; }

private:
    void mix(std::uint64_t v) noexcept {
        for (int i = 0; i < 8; ++i) {
            h_ ^= (v >> (i * 8)) & 0xFF;
            h_ *= 0x100000001b3ull;
        }
    }
    std::uint64_t h_     = 0xcbf29ce484222325ull;
    std::uint64_t count_ = 0;
};

}  // namespace ob
