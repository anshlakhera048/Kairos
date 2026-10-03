// Tests for pluggable latency models.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "kairos/latency.hpp"

namespace kairos {
namespace {

TEST(Latency, Constant) {
    ConstantLatency m{1500};
    std::uint64_t rng = 42;
    for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(m.sample(rng), 1500u);
    }
}

TEST(Latency, JitteredBounds) {
    JitteredLatency m{1000, 500};
    std::uint64_t rng = 1234;
    for (int i = 0; i < 1000; ++i) {
        const auto s = m.sample(rng);
        EXPECT_GE(s, 1000u);
        EXPECT_LE(s, 1500u);
    }
}

TEST(Latency, JitteredDeterministic) {
    JitteredLatency m{1000, 500};
    std::uint64_t r1 = 99, r2 = 99;
    for (int i = 0; i < 50; ++i) {
        EXPECT_EQ(m.sample(r1), m.sample(r2));
    }
}

TEST(Latency, JitteredZeroJitter) {
    JitteredLatency m{777, 0};
    std::uint64_t rng = 1;
    EXPECT_EQ(m.sample(rng), 777u);
}

TEST(Latency, EmpiricalEmpty) {
    EmpiricalLatency m;
    std::uint64_t rng = 5;
    EXPECT_EQ(m.sample(rng), 0u);
}

TEST(Latency, EmpiricalSingleValue) {
    const std::uint64_t s[] = {42000, 42000, 42000};
    EmpiricalLatency m(s, 3);
    std::uint64_t rng = 7;
    for (int i = 0; i < 20; ++i) {
        EXPECT_EQ(m.sample(rng), 42000u);
    }
}

TEST(Latency, EmpiricalRangeAndDeterminism) {
    // Bimodal samples: half near 1000, half near 9000.
    std::vector<std::uint64_t> samples;
    for (int i = 0; i < 500; ++i) {
        samples.push_back(1000 + static_cast<std::uint64_t>(i % 10));
        samples.push_back(9000 + static_cast<std::uint64_t>(i % 10));
    }
    EmpiricalLatency m(samples.data(), samples.size(), 32);
    std::uint64_t r1 = 2026, r2 = 2026;
    int low = 0, high = 0;
    for (int i = 0; i < 2000; ++i) {
        const auto a = m.sample(r1);
        const auto b = m.sample(r2);
        EXPECT_EQ(a, b);  // deterministic given the same rng state
        EXPECT_GE(a, 1000u);
        EXPECT_LE(a, 9010u);
        if (a < 5000) {
            ++low;
        } else {
            ++high;
        }
    }
    // Roughly half the samples should fall in each mode.
    EXPECT_GT(low, 500);
    EXPECT_GT(high, 500);
}

TEST(Latency, LatencyPair) {
    LatencyPair<ConstantLatency, JitteredLatency> p{{250}, {100, 50}};
    std::uint64_t rng = 11;
    EXPECT_EQ(p.feed.sample(rng), 250u);
    const auto o = p.order.sample(rng);
    EXPECT_GE(o, 100u);
    EXPECT_LE(o, 150u);
}

}  // namespace
}  // namespace kairos
