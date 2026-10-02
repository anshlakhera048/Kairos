#pragma once

// Limit order book: interface sketch.
//
// The full price-time-priority implementation lands in Phase 1. This header
// declares the public surface now so that callers, tests and benchmarks can
// be written against it; every method is a stub until then. See
// docs/design/orderbook.md (Phase 1) for the data-structure decisions.

#include "kairos/types.hpp"

namespace kairos {

class OrderBook {
public:
    OrderBook() = default;

    // The book is a unique resource: no copying, no moving of the pool.
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // Add a limit order. Returns false until Phase 1 implements matching.
    bool add(OrderId id, Side side, Price price, Quantity qty) noexcept;

    // Cancel a resting order. Returns false until Phase 1.
    bool cancel(OrderId id) noexcept;
};

}  // namespace kairos
