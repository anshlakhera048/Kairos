#pragma once

// Example bots for the Phase 4 arena.
//
// Each bot satisfies the Strategy concept (seven callbacks taking
// Context&). They work with both the Phase 3 Simulator and the Phase 4
// Arena, since the Context interface is the same.

#include <cstdint>

#include "kairos/fill_model.hpp"  // SimSide, SimFill
#include "kairos/l2_book.hpp"
#include "kairos/simulator.hpp"  // TradeInfo, AckInfo

namespace kairos {
namespace arena {
namespace bots {

// ---------------------------------------------------------------------------
// FixedSpreadBot: quotes at a fixed spread around the mid.
// ---------------------------------------------------------------------------

class FixedSpreadBot {
public:
    FixedSpreadBot(std::int64_t spread_ticks = 2,
                   std::int64_t size_lots = 1'000'000)
        : spread_ticks_(spread_ticks), size_lots_(size_lots) {}

    template <typename Ctx>
    void on_book_update(Ctx& ctx) {
        requote(ctx);
    }

    template <typename Ctx>
    void on_trade(Ctx&, const TradeInfo&) {}

    template <typename Ctx>
    void on_own_fill(Ctx& ctx, const SimFill&) {
        requote(ctx);
    }

    template <typename Ctx>
    void on_ack(Ctx&, const AckInfo&) {}

    template <typename Ctx>
    void on_reject(Ctx&, std::uint64_t) {}

    template <typename Ctx>
    void on_cancel(Ctx&, std::uint64_t) {}

    template <typename Ctx>
    void on_timer(Ctx& ctx) {
        requote(ctx);
    }

private:
    template <typename Ctx>
    void requote(Ctx& ctx) {
        L2Level bb, ba;
        if (!ctx.book().best_bid(bb) || !ctx.book().best_ask(ba)) return;
        const std::int64_t mid = (bb.price_ticks + ba.price_ticks) / 2;
        const std::int64_t bid_px = mid - spread_ticks_ / 2;
        const std::int64_t ask_px = mid + (spread_ticks_ - spread_ticks_ / 2);
        // Only requote if the desired prices changed (avoid churn).
        if (bid_px == last_bid_px_ && ask_px == last_ask_px_) return;
        last_bid_px_ = bid_px;
        last_ask_px_ = ask_px;
        if (bid_id_ != 0) ctx.cancel(bid_id_);
        if (ask_id_ != 0) ctx.cancel(ask_id_);
        bid_id_ = ctx.send_limit(SimSide::Bid, bid_px, size_lots_, true);
        ask_id_ = ctx.send_limit(SimSide::Ask, ask_px, size_lots_, true);
    }

    std::int64_t spread_ticks_;
    std::int64_t size_lots_;
    std::uint64_t bid_id_ = 0;
    std::uint64_t ask_id_ = 0;
    std::int64_t last_bid_px_ = 0;
    std::int64_t last_ask_px_ = 0;
};

// ---------------------------------------------------------------------------
// MomentumBot: buys when price is rising, sells when falling.
// ---------------------------------------------------------------------------

class MomentumBot {
public:
    explicit MomentumBot(std::int64_t size_lots = 1'000'000)
        : size_lots_(size_lots) {}

    template <typename Ctx>
    void on_book_update(Ctx& ctx) {
        L2Level bb, ba;
        if (!ctx.book().best_bid(bb) || !ctx.book().best_ask(ba)) return;
        const std::int64_t mid = (bb.price_ticks + ba.price_ticks) / 2;
        if (last_mid_ != 0) {
            // Simple momentum: if mid moved up, buy; down, sell.
            // (Taker orders — crosses the spread.)
            if (mid > last_mid_) {
                ctx.send_limit(SimSide::Bid, ba.price_ticks, size_lots_, false);
            } else if (mid < last_mid_) {
                ctx.send_limit(SimSide::Ask, bb.price_ticks, size_lots_, false);
            }
        }
        last_mid_ = mid;
    }

    template <typename Ctx>
    void on_trade(Ctx&, const TradeInfo&) {}
    template <typename Ctx>
    void on_own_fill(Ctx&, const SimFill&) {}
    template <typename Ctx>
    void on_ack(Ctx&, const AckInfo&) {}
    template <typename Ctx>
    void on_reject(Ctx&, std::uint64_t) {}
    template <typename Ctx>
    void on_cancel(Ctx&, std::uint64_t) {}
    template <typename Ctx>
    void on_timer(Ctx&) {}

private:
    std::int64_t size_lots_;
    std::int64_t last_mid_ = 0;
};

// ---------------------------------------------------------------------------
// RandomBot: baseline — sends random orders.
// ---------------------------------------------------------------------------

class RandomBot {
public:
    explicit RandomBot(std::uint64_t seed = 0, std::int64_t size_lots = 500'000)
        : rng_(seed), size_lots_(size_lots) {}

    template <typename Ctx>
    void on_timer(Ctx& ctx) {
        L2Level bb, ba;
        if (!ctx.book().best_bid(bb) || !ctx.book().best_ask(ba)) return;
        // Random side, random price within 5 ticks of touch.
        const bool is_bid = (rng_.next_u64() & 1) == 0;
        const std::int64_t dist =
            static_cast<std::int64_t>(rng_.next_u64() % 5);
        const std::int64_t px =
            is_bid ? bb.price_ticks - dist : ba.price_ticks + dist;
        if (px > 0) {
            ctx.send_limit(is_bid ? SimSide::Bid : SimSide::Ask, px, size_lots_,
                           true);
        }
    }

    template <typename Ctx>
    void on_book_update(Ctx&) {}
    template <typename Ctx>
    void on_trade(Ctx&, const TradeInfo&) {}
    template <typename Ctx>
    void on_own_fill(Ctx&, const SimFill&) {}
    template <typename Ctx>
    void on_ack(Ctx&, const AckInfo&) {}
    template <typename Ctx>
    void on_reject(Ctx&, std::uint64_t) {}
    template <typename Ctx>
    void on_cancel(Ctx&, std::uint64_t) {}

private:
    Rng rng_;
    std::int64_t size_lots_;
};

// ---------------------------------------------------------------------------
// CheaterBot: deliberately violates limits (for the anti-cheat test).
// ---------------------------------------------------------------------------
//
// Tries to exceed the position limit by sending a huge order. The arena
// must reject it. If the bot somehow gets the order through, the test
// fails.

class CheaterBot {
public:
    template <typename Ctx>
    void on_timer(Ctx& ctx) {
        L2Level bb, ba;
        if (!ctx.book().best_bid(bb) || !ctx.book().best_ask(ba)) return;
        // Try to buy 1B lots (far above the 10M limit).
        ctx.send_limit(SimSide::Bid, bb.price_ticks, 1'000'000'000LL, true);
    }

    template <typename Ctx>
    void on_book_update(Ctx&) {}
    template <typename Ctx>
    void on_trade(Ctx&, const TradeInfo&) {}
    template <typename Ctx>
    void on_own_fill(Ctx&, const SimFill&) {}
    template <typename Ctx>
    void on_ack(Ctx&, const AckInfo&) {}
    template <typename Ctx>
    void on_reject(Ctx&, std::uint64_t) {}
    template <typename Ctx>
    void on_cancel(Ctx&, std::uint64_t) {}
};

}  // namespace bots
}  // namespace arena
}  // namespace kairos
