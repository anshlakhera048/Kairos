#pragma once

// L2 order book reconstruction from recorded data.
//
// This is NOT the Phase 1 matching engine. The matching engine tracks
// individual orders with price-time priority. An L2 feed only publishes
// aggregate quantities per price level, so this book tracks
// price -> total quantity per level. Queue position within a level cannot
// be observed from L2 data and must be MODELLED (see
// docs/design/fill-model.md).
//
// Design: two sorted vectors (bids descending, asks ascending by price).
// Binary search for lookup; memmove for insert/erase. For ~1000 levels
// this is cache-friendly and keeps the project's "no std::map" rule
// absolute, even though this is offline batch work rather than the
// matching hot path.

#include <cstdint>
#include <vector>

#include "kairos/data_format.hpp"

namespace kairos {

struct L2Level {
    std::int64_t price_ticks;
    std::int64_t qty_lots;
};

class L2Book {
public:
    L2Book() = default;

    void clear();

    // Apply a snapshot event (replaces the whole book).
    void apply_snapshot(const data::EventView& ev);

    // Apply a diff event (level updates; qty 0 removes the level).
    void apply_diff(const data::EventView& ev);

    // Apply any event by type.
    void apply(const data::EventView& ev);

    bool best_bid(L2Level& out) const;
    bool best_ask(L2Level& out) const;

    // Quantity at an exact price level, or 0 if absent.
    std::int64_t level_qty(bool is_bid, std::int64_t price_ticks) const;

    // Set a level (for the arena's delayed view). Qty 0 removes the level.
    void upsert(bool is_bid, std::int64_t price, std::int64_t qty);

    std::size_t bid_count() const { return bids_.size(); }
    std::size_t ask_count() const { return asks_.size(); }

    // The book is never crossed (best bid < best ask) for valid feeds.
    bool check_invariants() const;

    // Level vectors for iteration (e.g., walking the book for taker fills).
    // Bids descending by price, asks ascending.
    const std::vector<L2Level>& bid_levels() const { return bids_; }
    const std::vector<L2Level>& ask_levels() const { return asks_; }

private:
    // Bids sorted descending by price (best first); asks ascending.
    std::vector<L2Level> bids_;
    std::vector<L2Level> asks_;

    static std::vector<L2Level>& side_vec(std::vector<L2Level>& bids,
                                          std::vector<L2Level>& asks,
                                          bool is_bid) {
        return is_bid ? bids : asks;
    }
};

}  // namespace kairos
