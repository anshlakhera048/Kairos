// Flow generator implementations for the Phase 4 arena.

#include "kairos/flow.hpp"

#include <cmath>
#include <limits>

namespace kairos {
namespace arena {

namespace {

// Helper: get mid price in ticks, or 0 if book is empty/one-sided.
std::int64_t mid_ticks(const Engine& engine) {
    Price bb(0), ba(0);
    if (engine.book().best_bid(bb) && engine.book().best_ask(ba)) {
        return (bb.ticks + ba.ticks) / 2;
    }
    return 0;
}

// Helper: submit an order, ignoring the events (generators don't need them).
void submit(Engine& engine, std::uint64_t id, Side side, OrderType type,
            std::int64_t price_ticks, std::int64_t qty_lots, std::uint64_t now,
            std::uint32_t owner) {
    Event out[16];
    engine.book().add(OrderId(id), side, type, Price(price_ticks),
                      Quantity(qty_lots), Timestamp(now), owner, out, 16);
}

}  // namespace

// ---------------------------------------------------------------------------
// NoiseTrader
// ---------------------------------------------------------------------------

NoiseTrader::NoiseTrader(const NoiseConfig& cfg, std::uint32_t owner)
    : cfg_(cfg), owner_(owner), rng_(cfg.seed) {
    next_time_ = static_cast<std::uint64_t>(rng_.next_exp(cfg.arrival_rate_per_s) * 1e9);
}

std::uint64_t NoiseTrader::act(Engine& engine, std::uint64_t now, Event* /*out*/,
                               std::size_t /*out_capacity*/) {
    const std::int64_t mid = mid_ticks(engine);
    if (mid > 0) {
        const bool is_market = rng_.next_unit() < cfg_.p_market;
        const Side side = rng_.next_unit() < 0.5 ? Side::Bid : Side::Ask;
        // Lognormal size.
        const double size = cfg_.mean_size_lots *
                            std::exp(cfg_.size_sigma * rng_.next_gauss());
        const std::int64_t qty = static_cast<std::int64_t>(size);
        const std::uint64_t id = (static_cast<std::uint64_t>(owner_) << 32) |
                                 (order_seq_++);

        if (is_market) {
            submit(engine, id, side, OrderType::Market, 0, qty, now, owner_);
        } else {
            // Limit at exponential distance from the touch.
            Price touch(0);
            const bool has_touch = (side == Side::Bid)
                                       ? engine.book().best_bid(touch)
                                       : engine.book().best_ask(touch);
            if (has_touch) {
                const std::int64_t dist = static_cast<std::int64_t>(
                    rng_.next_exp(1.0 / cfg_.price_distance_ticks));
                const std::int64_t px = (side == Side::Bid)
                                            ? touch.ticks - dist
                                            : touch.ticks + dist;
                if (px > 0) {
                    submit(engine, id, side, OrderType::Limit, px, qty, now,
                           owner_);
                    // Schedule a cancel (we don't track it precisely; the
                    // cancel rate is approximated by the next arrival being
                    // a cancel — simplified: just let them rest).
                }
            }
        }
    }
    // Next arrival.
    const double dt = rng_.next_exp(cfg_.arrival_rate_per_s);
    next_time_ = now + static_cast<std::uint64_t>(dt * 1e9);
    return next_time_;
}

// ---------------------------------------------------------------------------
// InformedTrader
// ---------------------------------------------------------------------------

InformedTrader::InformedTrader(const InformedConfig& cfg,
                               std::int64_t init_mid_ticks,
                               std::uint32_t owner)
    : cfg_(cfg),
      owner_(owner),
      rng_(cfg.seed),
      fair_(static_cast<double>(init_mid_ticks)),
      last_time_(0) {
    next_time_ = 0;  // Act immediately to initialize.
}

std::uint64_t InformedTrader::act(Engine& engine, std::uint64_t now,
                                  Event* /*out*/,
                                  std::size_t /*out_capacity*/) {
    // Advance the latent fair value from last_time_ to now.
    if (last_time_ > 0 && now > last_time_) {
        const double dt_s = (now - last_time_) / 1e9;
        // Brownian motion.
        fair_ += cfg_.sigma_v_per_sqrt_s * std::sqrt(dt_s) * 1e4 *
                 rng_.next_gauss();
        // Jumps (news).
        const double p_jump = cfg_.jump_rate_per_s * dt_s;
        if (rng_.next_unit() < p_jump) {
            fair_ += cfg_.jump_sigma * 1e4 * rng_.next_gauss();
        }
    }
    last_time_ = now;

    const std::int64_t mid = mid_ticks(engine);
    if (mid > 0) {
        const double diff = fair_ - static_cast<double>(mid);
        if (std::abs(diff) > cfg_.trade_threshold_ticks) {
            // Trade toward the fair value via market order.
            const Side side = diff > 0 ? Side::Bid : Side::Ask;
            const std::uint64_t id = (static_cast<std::uint64_t>(owner_) << 32) |
                                     (order_seq_++);
            submit(engine, id, side, OrderType::Market, 0,
                   cfg_.trade_size_lots, now, owner_);
            // Rate-limit: next trade after 1/rate seconds.
            const double dt = rng_.next_exp(cfg_.trade_rate_per_s);
            next_time_ = now + static_cast<std::uint64_t>(dt * 1e9);
            return next_time_;
        }
    }
    // No trade: check again soon (poll the fair value).
    next_time_ = now + 10'000'000;  // 10ms
    return next_time_;
}

// ---------------------------------------------------------------------------
// FixedSpreadMaker
// ---------------------------------------------------------------------------

FixedSpreadMaker::FixedSpreadMaker(const FixedSpreadConfig& cfg,
                                   std::uint32_t owner)
    : cfg_(cfg), owner_(owner), rng_(cfg.seed) {
    next_time_ = 0;
}

std::uint64_t FixedSpreadMaker::act(Engine& engine, std::uint64_t now,
                                   Event* /*out*/,
                                   std::size_t /*out_capacity*/) {
    // Cancel old quotes.
    if (bid_id_ != 0) {
        Event out[4];
        engine.book().cancel(OrderId(bid_id_), Timestamp(now), out, 4);
        bid_id_ = 0;
    }
    if (ask_id_ != 0) {
        Event out[4];
        engine.book().cancel(OrderId(ask_id_), Timestamp(now), out, 4);
        ask_id_ = 0;
    }

    const std::int64_t mid = mid_ticks(engine);
    if (mid > 0) {
        const std::int64_t half = cfg_.spread_ticks / 2;
        const std::int64_t bid_px = mid - half;
        const std::int64_t ask_px = mid + (cfg_.spread_ticks - half);
        if (bid_px > 0 && ask_px > bid_px) {
            bid_id_ = (static_cast<std::uint64_t>(owner_) << 32) | (order_seq_++);
            ask_id_ = (static_cast<std::uint64_t>(owner_) << 32) | (order_seq_++);
            submit(engine, bid_id_, Side::Bid, OrderType::PostOnly, bid_px,
                   cfg_.size_lots, now, owner_);
            submit(engine, ask_id_, Side::Ask, OrderType::PostOnly, ask_px,
                   cfg_.size_lots, now, owner_);
        }
    }
    next_time_ = now + cfg_.refresh_interval_ns;
    return next_time_;
}

// ---------------------------------------------------------------------------
// MomentumTrader
// ---------------------------------------------------------------------------

MomentumTrader::MomentumTrader(const MomentumConfig& cfg, std::uint32_t owner)
    : cfg_(cfg), owner_(owner), rng_(cfg.seed) {
    next_time_ = 0;
}

std::uint64_t MomentumTrader::act(Engine& engine, std::uint64_t now,
                                 Event* /*out*/,
                                 std::size_t /*out_capacity*/) {
    const std::int64_t mid = mid_ticks(engine);
    if (mid > 0) {
        if (last_time_ > 0 && now > last_time_) {
            const double dt_s = (now - last_time_) / 1e9;
            // EWMA update: trend = alpha * (mid - last_mid) + (1 - alpha) * trend
            const double alpha = 1.0 - std::exp(-dt_s * 0.693147 /
                                                cfg_.trend_halflife_s);
            trend_ = alpha * (static_cast<double>(mid - last_mid_)) +
                     (1.0 - alpha) * trend_;
        }
        last_mid_ = mid;
        last_time_ = now;

        if (std::abs(trend_) > cfg_.trade_threshold) {
            const Side side = trend_ > 0 ? Side::Bid : Side::Ask;
            const std::uint64_t id = (static_cast<std::uint64_t>(owner_) << 32) |
                                     (order_seq_++);
            submit(engine, id, side, OrderType::Market, 0,
                   cfg_.trade_size_lots, now, owner_);
            const double dt = rng_.next_exp(cfg_.trade_rate_per_s);
            next_time_ = now + static_cast<std::uint64_t>(dt * 1e9);
            return next_time_;
        }
    }
    next_time_ = now + 50'000'000;  // 50ms poll
    return next_time_;
}

}  // namespace arena
}  // namespace kairos
