#pragma once

// Matching engine: owns the book, assigns deterministic sequence numbers,
// emits events. The ownership structure: the engine owns exactly one book,
// single-threaded; all time arrives as an explicit parameter.

#include "kairos/order_book.hpp"

namespace kairos {

class Engine {
public:
    Engine() = default;
    explicit Engine(OrderBookConfig cfg) : book_(cfg) {}

    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    OrderBook& book() noexcept { return book_; }
    const OrderBook& book() const noexcept { return book_; }

private:
    OrderBook book_;
};

}  // namespace kairos
