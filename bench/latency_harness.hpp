// Minimal HDR-style latency histogram for the benchmark harness.
//
// Records nanosecond latencies into buckets: 1ns linear resolution below
// 2048ns, then 64 sub-buckets per power-of-two octave up to 2^61 ns, plus a
// final overflow bucket. Header-only, noexcept, no allocation after
// construction. percentile() returns the bucket's inclusive upper bound
// (conservative: the true latency is <= the reported value).

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>

namespace kairos {
namespace bench {

class LatencyHistogram {
public:
    static constexpr std::size_t kLinearBuckets = 2048;
    static constexpr unsigned kMinLogExp = 11;       // 2^11 == 2048
    static constexpr unsigned kMaxLogExp = 60;       // covers up to 2^61 ns
    static constexpr std::size_t kLogBuckets =
        (kMaxLogExp - kMinLogExp + 1) * 64;
    static constexpr std::size_t kNumBuckets =
        kLinearBuckets + kLogBuckets + 1;  // +1 overflow bucket

    LatencyHistogram() { reset(); }

    void reset() noexcept {
        for (std::size_t i = 0; i < kNumBuckets; ++i) {
            buckets_[i] = 0;
        }
        count_ = 0;
        total_ = 0;
        min_ = UINT64_MAX;
        max_ = 0;
    }

    void record(std::uint64_t ns) noexcept {
        buckets_[bucket_index(ns)]++;
        ++count_;
        total_ += ns;
        if (ns < min_) {
            min_ = ns;
        }
        if (ns > max_) {
            max_ = ns;
        }
    }

    // p in (0, 100]. Returns 0 when empty.
    std::uint64_t percentile(double p) const noexcept {
        if (count_ == 0 || p <= 0.0) {
            return 0;
        }
        std::uint64_t target =
            static_cast<std::uint64_t>(p / 100.0 * static_cast<double>(count_));
        if (target == 0) {
            target = 1;
        }
        if (target > count_) {
            target = count_;
        }
        std::uint64_t acc = 0;
        for (std::size_t i = 0; i < kNumBuckets; ++i) {
            acc += buckets_[i];
            if (acc >= target) {
                return bucket_upper(i);
            }
        }
        return max_;
    }

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t min() const noexcept { return count_ ? min_ : 0; }
    std::uint64_t max() const noexcept { return max_; }
    double mean() const noexcept {
        return count_ ? static_cast<double>(total_) / count_ : 0.0;
    }

    static constexpr std::size_t bucket_index(std::uint64_t v) noexcept {
        if (v < kLinearBuckets) {
            return static_cast<std::size_t>(v);
        }
        const unsigned e = 63 - std::countl_zero(v);
        if (e > kMaxLogExp) {
            return kNumBuckets - 1;  // overflow bucket
        }
        const std::uint64_t sub = (v >> (e - 6)) & 63;
        return kLinearBuckets + (e - kMinLogExp) * 64 +
               static_cast<std::size_t>(sub);
    }

    static std::uint64_t bucket_upper(std::size_t idx) noexcept {
        if (idx < kLinearBuckets) {
            return static_cast<std::uint64_t>(idx);
        }
        if (idx == kNumBuckets - 1) {
            return UINT64_MAX;
        }
        const std::size_t k = idx - kLinearBuckets;
        const unsigned e =
            kMinLogExp + static_cast<unsigned>(k / 64);  // <= kMaxLogExp
        const std::uint64_t sub = k % 64;
        // Bucket (e, sub) covers [(64+sub) << (e-6), (65+sub) << (e-6)).
        return ((65 + sub) << (e - 6)) - 1;
    }

private:
    std::uint64_t buckets_[kNumBuckets];
    std::uint64_t count_ = 0;
    std::uint64_t total_ = 0;
    std::uint64_t min_ = UINT64_MAX;
    std::uint64_t max_ = 0;
};

}  // namespace bench
}  // namespace kairos
