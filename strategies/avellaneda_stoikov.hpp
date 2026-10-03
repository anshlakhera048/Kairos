#pragma once

// Avellaneda-Stoikov market-making strategy for the Kairos simulator.
//
// On every timer (and book update), it:
//   1. Reads the mid price from the delayed book.
//   2. Reads inventory from the account.
//   3. Computes A-S optimal quotes.
//   4. Cancels stale quotes and places new ones (post-only).
//
// Inventory limits: if |position| >= max_inventory_lots, it only quotes
// the side that reduces inventory. Fast-market guard: if the spread
// exceeds max_spread_ticks, it pulls quotes (no quoting into a gappy
// book).
//
// Callbacks are templates on the context type, so the strategy works with
// any Simulator instantiation (no circular type dependency).

#include <cstdint>

#include "kairos/as_model.hpp"
#include "kairos/fill_model.hpp"  // SimSide, SimFill
#include "kairos/l2_book.hpp"

namespace kairos {

struct ASStrategyConfig {
    ASParams model;
    std::int64_t order_size_lots = 1000000;      // per quote
    std::int64_t max_inventory_lots = 10000000;  // hard limit
    std::int64_t max_spread_ticks = 500;         // fast-market guard
};

class ASStrategy {
public:
    explicit ASStrategy(const ASStrategyConfig& cfg) : cfg_(cfg) {}

    template <typename Ctx>
    void on_book_update(Ctx& ctx) {
        maybe_requote(ctx);
    }

    template <typename Ctx>
    void on_trade(Ctx&, const TradeInfo&) {}

    template <typename Ctx>
    void on_own_fill(Ctx& ctx, const SimFill&) {
        // Inventory changed; requote immediately.
        maybe_requote(ctx);
    }

    template <typename Ctx>
    void on_ack(Ctx&, const AckInfo&) {}
    template <typename Ctx>
    void on_reject(Ctx&, std::uint64_t) {}
    template <typename Ctx>
    void on_cancel(Ctx&, std::uint64_t) {}

    template <typename Ctx>
    void on_timer(Ctx& ctx) {
        maybe_requote(ctx);
    }

    // For tests/introspection.
    std::uint64_t bid_id() const { return bid_id_; }
    std::uint64_t ask_id() const { return ask_id_; }

private:
    ASStrategyConfig cfg_;
    std::uint64_t bid_id_ = 0;
    std::uint64_t ask_id_ = 0;
    std::uint64_t start_ns_ = 0;
    bool started_ = false;

    template <typename Ctx>
    void maybe_requote(Ctx& ctx) {
        if (!started_) {
            start_ns_ = ctx.now();
            started_ = true;
        }
        L2Level bb, ba;
        if (!ctx.book().best_bid(bb) || !ctx.book().best_ask(ba)) {
            return;
        }
        const std::int64_t spread = ba.price_ticks - bb.price_ticks;
        if (spread > cfg_.max_spread_ticks) {
            // Fast market: pull quotes.
            pull(ctx);
            return;
        }
        const std::int64_t mid = bb.price_ticks + spread / 2;
        const std::int64_t inventory = ctx.account().position_lots;
        const double elapsed_s =
            static_cast<double>(ctx.now() - start_ns_) / 1e9;

        const bool too_long = inventory >= cfg_.max_inventory_lots;
        const bool too_short = inventory <= -cfg_.max_inventory_lots;

        const ASQuotes q = avellaneda_stoikov_quotes(
            cfg_.model, mid, inventory, elapsed_s);

        // Cancel stale quotes.
        pull(ctx);

        // Clamp to the touch: in backtest, trades only print at prices
        // where the historical book had liquidity. Quoting inside the
        // spread would never be hit by the recorded flow (assumption: my
        // orders don't move the market). A live strategy would join or
        // improve; here we join.
        std::int64_t bid_px = q.bid_ticks;
        std::int64_t ask_px = q.ask_ticks;
        if (bid_px > bb.price_ticks) {
            bid_px = bb.price_ticks;
        }
        if (ask_px < ba.price_ticks) {
            ask_px = ba.price_ticks;
        }

        // Place new quotes (post-only). If at the inventory limit, only
        // quote the reducing side.
        if (!too_long) {
            bid_id_ = ctx.send_limit(SimSide::Bid, bid_px,
                                     cfg_.order_size_lots, true);
        }
        if (!too_short) {
            ask_id_ = ctx.send_limit(SimSide::Ask, ask_px,
                                     cfg_.order_size_lots, true);
        }
    }

    template <typename Ctx>
    void pull(Ctx& ctx) {
        if (bid_id_ != 0) {
            ctx.cancel(bid_id_);
            bid_id_ = 0;
        }
        if (ask_id_ != 0) {
            ctx.cancel(ask_id_);
            ask_id_ = 0;
        }
    }
};

}  // namespace kairos
