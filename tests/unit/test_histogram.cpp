// Unit tests for the HDR-style latency histogram.

#include <gtest/gtest.h>

#include <cstdint>

#include "latency_harness.hpp"

namespace kairos {
namespace bench {
namespace {

TEST(LatencyHistogram, Empty) {
    LatencyHistogram h;
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.percentile(50), 0u);
    EXPECT_EQ(h.min(), 0u);
    EXPECT_EQ(h.max(), 0u);
}

TEST(LatencyHistogram, LinearRegionExact) {
    LatencyHistogram h;
    for (std::uint64_t v = 1; v <= 1000; ++v) {
        h.record(v);
    }
    EXPECT_EQ(h.count(), 1000u);
    EXPECT_EQ(h.min(), 1u);
    EXPECT_EQ(h.max(), 1000u);
    // Linear buckets are exact: percentile returns the bucket's value.
    EXPECT_EQ(h.percentile(50), 500u);
    EXPECT_EQ(h.percentile(99), 990u);
    EXPECT_EQ(h.percentile(100), 1000u);
}

TEST(LatencyHistogram, LogRegionConservative) {
    LatencyHistogram h;
    h.record(100000);  // 100us, in the log region
    const std::uint64_t p50 = h.percentile(50);
    EXPECT_GE(p50, 100000u);  // conservative: upper bound >= true value
    EXPECT_LT(p50, 110000u);  // but within ~10% (64 sub-buckets/octave ~1.1%)
    EXPECT_EQ(h.max(), 100000u);
    EXPECT_EQ(h.min(), 100000u);
}

TEST(LatencyHistogram, BucketRoundTrip) {
    // Every value lands in a bucket whose [lower, upper] contains it.
    for (std::uint64_t v :
         {0ULL, 1ULL, 2047ULL, 2048ULL, 3000ULL, 100000ULL, 1000000000ULL,
          (1ULL << 40)}) {
        const std::size_t idx = LatencyHistogram::bucket_index(v);
        const std::uint64_t upper = LatencyHistogram::bucket_upper(idx);
        EXPECT_GE(upper, v) << "v=" << v;
        // Lower bound check via the previous bucket's upper.
        if (idx > 0 && idx < LatencyHistogram::kNumBuckets - 1) {
            EXPECT_LT(LatencyHistogram::bucket_upper(idx - 1), v) << "v=" << v;
        }
    }
}

TEST(LatencyHistogram, OverflowBucket) {
    LatencyHistogram h;
    h.record(UINT64_MAX);
    EXPECT_EQ(h.percentile(100), UINT64_MAX);
    EXPECT_EQ(h.count(), 1u);
}

TEST(LatencyHistogram, Reset) {
    LatencyHistogram h;
    h.record(42);
    h.reset();
    EXPECT_EQ(h.count(), 0u);
    EXPECT_EQ(h.percentile(99), 0u);
}

}  // namespace
}  // namespace bench
}  // namespace kairos
