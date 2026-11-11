#pragma once
// HdrHistogram-style log-linear latency histogram (in-repo, dependency free).
//
// Values are bucketed by power of two, and each power of two is split into
// kSub linear sub-buckets, so relative error is bounded (~1/kSub = 0.8% at 128)
// across the whole range. Recording is O(1) with no allocation.
// Mean/max are exact; percentiles are accurate to the bucket width.
// If you want the real thing, HdrHistogram_c is a drop-in replacement.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace ob {

class LatencyHistogram {
public:
    static constexpr int kSubBits = 7;                 // 128 sub-buckets
    static constexpr int kSub     = 1 << kSubBits;
    static constexpr int kMags    = 64 - kSubBits;     // covers the full uint64 range

    void record(std::uint64_t v) noexcept {
        ++counts_[bucketOf(v)];
        ++total_;
        sum_ += static_cast<double>(v);
        max_ = std::max(max_, v);
        min_ = std::min(min_, v);
    }

    void merge(const LatencyHistogram& o) noexcept {
        for (std::size_t i = 0; i < counts_.size(); ++i) counts_[i] += o.counts_[i];
        total_ += o.total_;
        sum_ += o.sum_;
        max_ = std::max(max_, o.max_);
        min_ = std::min(min_, o.min_);
    }

    // p in [0, 100]. Returns the upper edge of the bucket containing the p-th percentile.
    std::uint64_t percentile(double p) const noexcept {
        if (total_ == 0) return 0;
        const auto target = static_cast<std::uint64_t>(std::ceil(p / 100.0 * static_cast<double>(total_)));
        std::uint64_t seen = 0;
        for (std::size_t i = 0; i < counts_.size(); ++i) {
            seen += counts_[i];
            if (seen >= std::max<std::uint64_t>(target, 1)) return std::min(upperEdge(i), max_);
        }
        return max_;
    }

    std::uint64_t count() const noexcept { return total_; }
    double        mean() const noexcept { return total_ ? sum_ / static_cast<double>(total_) : 0.0; }
    std::uint64_t max() const noexcept { return max_; }
    std::uint64_t min() const noexcept { return total_ ? min_ : 0; }

    void print(const char* label, const char* unit = "ns") const {
        std::printf("%-22s n=%-10llu min=%llu%s p50=%llu%s p90=%llu%s p99=%llu%s p99.9=%llu%s p99.99=%llu%s max=%llu%s mean=%.1f%s\n",
                    label, (unsigned long long)total_, (unsigned long long)min(), unit,
                    (unsigned long long)percentile(50), unit, (unsigned long long)percentile(90), unit,
                    (unsigned long long)percentile(99), unit, (unsigned long long)percentile(99.9), unit,
                    (unsigned long long)percentile(99.99), unit, (unsigned long long)max_, unit, mean(), unit);
    }

private:
    static std::size_t bucketOf(std::uint64_t v) noexcept {
        if (v < static_cast<std::uint64_t>(kSub)) return static_cast<std::size_t>(v);  // exact for small values
        const int msb   = 63 - __builtin_clzll(v);      // >= kSubBits
        const int shift = msb - kSubBits + 1;
        const std::uint64_t sub = v >> shift;           // in [kSub/2, kSub)
        return static_cast<std::size_t>(shift) * (kSub / 2) + kSub / 2 + (sub - kSub / 2);
    }

    static std::uint64_t upperEdge(std::size_t b) noexcept {
        if (b < static_cast<std::size_t>(kSub)) return b;
        const std::size_t shift = (b - kSub / 2) / (kSub / 2);
        const std::uint64_t sub = (b - kSub / 2) % (kSub / 2) + kSub / 2;
        if (shift >= 63) return ~0ull;
        return ((sub + 1) << shift) - 1;
    }

    std::array<std::uint64_t, (kMags + 1) * (kSub / 2) + kSub> counts_{};
    std::uint64_t total_ = 0;
    double        sum_   = 0;
    std::uint64_t max_   = 0;
    std::uint64_t min_   = ~0ull;
};

}  // namespace ob
