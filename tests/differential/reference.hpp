// Slow, obviously-correct reference order book for differential testing.
//
// Deliberately uses std::map + std::list: O(log n) and pointer-chasing, but
// trivially auditable. It mirrors the engine's documented event-stream
// contract EXACTLY (see docs/design/orderbook.md). Any divergence between
// this and OrderBook is a bug in one of them. Used ONLY by tests.

#pragma once

#include <cstdint>
#include <list>
#include <map>
#include <unordered_map>
#include <vector>

#include "kairos/error.hpp"
#include "kairos/events.hpp"
#include "kairos/order_book.hpp"
#include "kairos/types.hpp"

namespace kairos {
namespace reference {

struct RefOrder {
    OrderId id{0};
    Price price{0};
    Quantity qty{0};
    std::uint64_t seq = 0;
    std::uint32_t owner = 0;
    Side side = Side::Bid;
    OrderType type = OrderType::Limit;
};

struct RefConfig {
    Price base_price{0};
    std::uint32_t half_band = 1024;
    std::uint32_t max_orders = 1u << 20;
    SelfTradePolicy self_trade = SelfTradePolicy::Allow;
};

class RefBook {
public:
    using OpResult = OrderBook::OpResult;

    explicit RefBook(RefConfig cfg = RefConfig{}) : cfg_(cfg) {}

    OpResult add(OrderId id, Side side, OrderType type, Price price, Quantity qty,
                 Timestamp ts, std::uint32_t owner, Event* out,
                 std::size_t out_capacity) noexcept;
    OpResult cancel(OrderId id, Timestamp ts, Event* out,
                    std::size_t out_capacity) noexcept;
    OpResult modify(OrderId id, Price new_price, Quantity new_qty, Timestamp ts,
                    Event* out, std::size_t out_capacity) noexcept;

    std::size_t resting_count() const { return live_; }

private:
    struct Loc {
        Side side;
        std::int64_t price_ticks;
        std::list<RefOrder>::iterator it;
    };

    bool in_band(Price p) const {
        const std::int64_t lo = cfg_.base_price.ticks - cfg_.half_band;
        return p.ticks >= lo && p.ticks < lo + 2 * cfg_.half_band;
    }
    std::map<std::int64_t, std::list<RefOrder>>& book(Side s) {
        return s == Side::Bid ? bids_ : asks_;
    }

    void emit(EventType type, OrderId id, Timestamp ts, Price price, Quantity qty,
              Event* out, std::size_t& n) {
        out[n].type = type;
        out[n].order_id = id;
        out[n].ts = ts;
        out[n].seq = next_event_seq_++;
        out[n].price = price;
        out[n].qty = qty;
        ++n;
    }

    // Shared match+rest core; see OrderBook::execute_add for the contract.
    Error execute_add(OrderId id, Side side, OrderType type, Price price,
                      std::uint64_t qty_lots, Timestamp ts, std::uint32_t owner,
                      Event* out, std::size_t& n);

    std::uint64_t fillable(Side taker_side, Price limit, std::uint64_t need,
                           std::uint32_t owner) const;

    RefConfig cfg_;
    std::map<std::int64_t, std::list<RefOrder>> bids_;  // ticks -> FIFO (ascending)
    std::map<std::int64_t, std::list<RefOrder>> asks_;
    std::unordered_map<std::uint64_t, Loc> by_id_;
    std::uint64_t next_event_seq_ = 1;
    std::uint64_t next_order_seq_ = 1;
    std::size_t live_ = 0;
};

}  // namespace reference
}  // namespace kairos
