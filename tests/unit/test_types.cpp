// Smoke tests for the core value types.
// Proves the toolchain end to end: headers compile, gtest links, ctest runs.
// The static_asserts are the real test here: the "zero-cost wrapper" claim
// from types.hpp must hold on every compiler we support.

#include <gtest/gtest.h>

#include <cstdint>

#include "kairos/types.hpp"

namespace kairos {
namespace {

TEST(Types, WrappersAreZeroCost) {
    static_assert(sizeof(Price) == sizeof(std::int64_t));
    static_assert(sizeof(Quantity) == sizeof(std::int64_t));
    static_assert(sizeof(OrderId) == sizeof(std::uint64_t));
    static_assert(sizeof(Timestamp) == sizeof(std::uint64_t));
    static_assert(sizeof(Side) == 1);
    SUCCEED();
}

TEST(Types, Comparisons) {
    EXPECT_LT(Price{100}, Price{200});
    EXPECT_EQ(Price{100}, Price{100});
    EXPECT_GT(Quantity{5}, Quantity{3});
    EXPECT_NE(OrderId{1}, OrderId{2});
    EXPECT_LT(Timestamp{10}, Timestamp{20});
}

TEST(Types, SideHelpers) {
    EXPECT_EQ(opposite(Side::Bid), Side::Ask);
    EXPECT_EQ(opposite(Side::Ask), Side::Bid);
    // Bid = 0 / Ask = 1: usable directly as an array index.
    EXPECT_EQ(static_cast<std::uint8_t>(Side::Bid), 0);
    EXPECT_EQ(static_cast<std::uint8_t>(Side::Ask), 1);
}

TEST(Types, OrderIdValidity) {
    EXPECT_FALSE(OrderId{0}.valid());
    EXPECT_TRUE(OrderId{1}.valid());
}

}  // namespace
}  // namespace kairos
