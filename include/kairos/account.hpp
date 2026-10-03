#pragma once

// Simulated account: positions, cash, realized/unrealized PnL, fees.
//
// All money is in integer lots (quote currency × qty_scale); prices are
// integer ticks. Fee rates are integer basis points and may be negative
// (maker rebates).
//
// Inventory accounting: average entry price for the current position.
// Realized PnL is booked when |position| is reduced.

#include <cstdint>

#include "kairos/fill_model.hpp"  // SimSide

namespace kairos {

struct Account {
    std::int64_t cash_lots = 0;        // quote currency lots
    std::int64_t position_lots = 0;    // base lots, signed (+ = long)
    std::int64_t avg_entry_ticks = 0;  // average entry price of position
    std::int64_t realized_pnl_lots = 0;
    std::int64_t total_fees_lots = 0;

    std::int64_t maker_fee_bp = 0;  // may be negative (rebate)
    std::int64_t taker_fee_bp = 0;

    // Apply a fill. side: the side MY order was on (Bid = I bought).
    void apply_fill(SimSide side, std::int64_t price_ticks,
                    std::int64_t qty_lots, bool is_maker);

    // Mark-to-market PnL of the open position.
    std::int64_t unrealized_pnl_lots(std::int64_t mark_price_ticks) const;

    // Total equity: cash + position marked at mark_price.
    std::int64_t equity_lots(std::int64_t mark_price_ticks) const;
};

}  // namespace kairos
