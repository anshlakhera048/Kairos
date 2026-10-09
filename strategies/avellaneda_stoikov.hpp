#pragma once

// Avellaneda-Stoikov market-making strategy for the Kairos simulator.
//
// On every timer (and book update), it:
//   1. Reads the mid price from the delayed book.
//   2. Reads inventory from the account.
//   3. Computes A-S optimal quotes.
//   4. Cancels stale quotes and places new ones (post-only) — but only
//      when the desired quotes actually changed (requote gating). Without
//      this, every book update churns two orders through the book: in the
//      arena that hammers the order-rate anti-cheat limit and the quotes
//      never rest long enough to be hit.
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
    // Per-quote size, in ENGINE MICRO-LOTS (not whole lots): 1'000'000
    // micro-lots = 1 whole lot. The A-S model itself takes inventory in
    // whole lots — see inventory_whole_lots() for the conversion.
    std::int64_t order_size_lots = 1000000;      // per quote
    // Hard inventory limit, in ENGINE MICRO-LOTS (not whole lots).
    std::int64_t max_inventory_lots = 10000000;  // hard limit
    std::int64_t max_spread_ticks = 500;         // fast-market guard
    // Engine lots per whole lot (account positions are in engine lots;
    // the A-S model takes inventory in whole lots, so we convert).
    double qty_scale = 1'000'000.0;
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
    void on_reject(Ctx&, std::uint64_t id) {
        // A rejected quote is not resting: forget it so the next requote
        // re-places instead of assuming it is live.
        if (id == bid_id_) {
            bid_id_ = 0;
            last_bid_px_ = 0;
        }
        if (id == ask_id_) {
            ask_id_ = 0;
            last_ask_px_ = 0;
        }
    }

    template <typename Ctx>
    void on_cancel(Ctx&, std::uint64_t id) {
        // Only forget the currently-tracked quote: a cancel ack for an
        // older, already-replaced quote must not wipe live state.
        if (id == bid_id_) {
            bid_id_ = 0;
            last_bid_px_ = 0;
        }
        if (id == ask_id_) {
            ask_id_ = 0;
            last_ask_px_ = 0;
        }
    }

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
    // Prices of the quotes we believe are currently resting (0 = that
    // side is not quoted). Drives requote gating: if the desired quotes
    // match these, there is nothing to do.
    std::int64_t last_bid_px_ = 0;
    std::int64_t last_ask_px_ = 0;
    // A send returning 0 was blocked by an anti-cheat limit. Back off and
    // retry no earlier than retry_after_ns_ (timer cadence).
    bool send_blocked_ = false;
    std::uint64_t retry_after_ns_ = 0;
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
            cfg_.model, mid, inventory_whole_lots(ctx), elapsed_s);

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

        // Requote gating: only cancel/re-place when the desired quotes
        // actually changed. If at the inventory limit, only quote the
        // reducing side (0 = not quoting that side).
        const std::int64_t want_bid = too_long ? 0 : bid_px;
        const std::int64_t want_ask = too_short ? 0 : ask_px;
        const bool changed =
            (want_bid != last_bid_px_ || want_ask != last_ask_px_);
        // If a previous send was blocked by an anti-cheat limit, retry on
        // the timer cadence — not on every book update.
        const bool retry_due = send_blocked_ && ctx.now() >= retry_after_ns_;
        if (!changed && !retry_due) {
            return;
        }
        send_blocked_ = false;

        // Cancel stale quotes.
        pull(ctx);

        // Place new quotes (post-only). A send returning 0 was blocked by
        // a rate/position limit (no callback follows); back off instead of
        // spinning against the limit on every book update.
        if (want_bid != 0) {
            bid_id_ = ctx.send_limit(SimSide::Bid, bid_px,
                                     cfg_.order_size_lots, true);
            if (bid_id_ == 0) {
                flag_blocked(ctx.now());
            }
        }
        if (want_ask != 0) {
            ask_id_ = ctx.send_limit(SimSide::Ask, ask_px,
                                     cfg_.order_size_lots, true);
            if (ask_id_ == 0) {
                flag_blocked(ctx.now());
            }
        }
        last_bid_px_ = want_bid;
        last_ask_px_ = want_ask;
    }

    void flag_blocked(std::uint64_t now) {
        send_blocked_ = true;
        retry_after_ns_ = now + 500'000'000ULL;  // retry on timer cadence
    }

    // The A-S model takes inventory in whole lots; engine positions are in
    // micro-lots (qty_scale per lot). Passing micro-lots directly makes the
    // inventory skew 1e6x too strong: one fill would throw quotes hundreds
    // of thousands of ticks from the touch.
    template <typename Ctx>
    double inventory_whole_lots(Ctx& ctx) const {
        return static_cast<double>(ctx.account().position_lots) /
               cfg_.qty_scale;
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
        last_bid_px_ = 0;
        last_ask_px_ = 0;
    }
};

}  // namespace kairos
