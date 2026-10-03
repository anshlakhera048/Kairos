// Simulated account implementation.
//
// Notional intermediates (qty_lots × price_ticks) use __int128 so they
// cannot overflow. Both supported compilers (GCC, Clang) provide it;
// the pragma below silences -Wpedantic's ISO-C++ complaint for this
// translation unit only.

#include "kairos/account.hpp"

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif

namespace kairos {

void Account::apply_fill(SimSide side, std::int64_t price_ticks,
                         std::int64_t qty_lots, bool is_maker) {
    const std::int64_t fee_bp = is_maker ? maker_fee_bp : taker_fee_bp;
    const __int128 notional = static_cast<__int128>(qty_lots) * price_ticks;
    const std::int64_t fee =
        static_cast<std::int64_t>(notional * fee_bp / 10000);
    total_fees_lots += fee;

    const std::int64_t signed_qty = side == SimSide::Bid ? qty_lots : -qty_lots;
    // Buying spends cash (plus fee); selling receives cash (minus fee).
    // A negative fee_bp (maker rebate) reverses the fee's sign.
    const std::int64_t notional64 = static_cast<std::int64_t>(notional);
    if (side == SimSide::Bid) {
        cash_lots -= notional64 + fee;
    } else {
        cash_lots += notional64 - fee;
    }

    // Inventory + realized PnL (average cost).
    if (position_lots == 0) {
        position_lots = signed_qty;
        avg_entry_ticks = price_ticks;
        return;
    }
    const bool same_direction = (position_lots > 0) == (signed_qty > 0);
    if (same_direction) {
        const std::int64_t abs_pos =
            position_lots >= 0 ? position_lots : -position_lots;
        const __int128 new_notional =
            static_cast<__int128>(abs_pos) * avg_entry_ticks + notional;
        const std::int64_t new_pos = position_lots + signed_qty;
        const std::int64_t abs_new = new_pos >= 0 ? new_pos : -new_pos;
        avg_entry_ticks =
            abs_new != 0 ? static_cast<std::int64_t>(new_notional / abs_new) : 0;
        position_lots = new_pos;
        return;
    }
    // Reducing (or flipping) the position: book realized PnL.
    const std::int64_t abs_pos = position_lots >= 0 ? position_lots : -position_lots;
    const std::int64_t abs_qty = signed_qty >= 0 ? signed_qty : -signed_qty;
    const std::int64_t closing = abs_qty < abs_pos ? abs_qty : abs_pos;
    // Long closed by selling: profit = (price - entry) * qty.
    // Short closed by buying: profit = (entry - price) * qty.
    const std::int64_t pnl_per_lot = position_lots > 0
                                         ? price_ticks - avg_entry_ticks
                                         : avg_entry_ticks - price_ticks;
    realized_pnl_lots +=
        static_cast<std::int64_t>(static_cast<__int128>(closing) * pnl_per_lot);
    position_lots += signed_qty;
    if (position_lots == 0) {
        avg_entry_ticks = 0;
    } else if (abs_qty > abs_pos) {
        // Flipped: the remainder opens a new position at this price.
        avg_entry_ticks = price_ticks;
    }
}

std::int64_t Account::unrealized_pnl_lots(
    std::int64_t mark_price_ticks) const {
    if (position_lots == 0) {
        return 0;
    }
    // (mark - entry) * position (signed) is correct for long and short.
    return static_cast<std::int64_t>(static_cast<__int128>(position_lots) *
                                     (mark_price_ticks - avg_entry_ticks));
}

std::int64_t Account::equity_lots(std::int64_t mark_price_ticks) const {
    const __int128 pos_value =
        static_cast<__int128>(position_lots) * mark_price_ticks;
    return cash_lots + static_cast<std::int64_t>(pos_value);
}

}  // namespace kairos

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
