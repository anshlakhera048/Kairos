#pragma once

// Queue-position and fill models for the simulator.
//
// L2 market data publishes aggregate quantities per price level, NOT
// individual orders. When my limit order rests at a price, I know the
// total level quantity but NOT how much is ahead of me vs behind me.
// Queue position must be MODELLED. Every assumption is documented in
// docs/design/fill-model.md with its known failure modes.
//
// Models (all satisfy the QueueModel concept; the simulator is templated
// on the model type — no virtual calls):
//
//   RiskAverseQueue: assumes I am at the BACK of the queue
//     (qty_ahead = level_qty - my_qty). Only TRADES advance me: a level
//     reduction not explained by trades is assumed to be cancels behind
//     me (equivalently: ignored). Pessimistic fill probabilities.
//
//   ProbabilisticQueue: a level reduction of D lots (not from trades) is
//     attributed to cancels ahead of me vs behind me by sampling
//     Binomial(D, p_ahead) with the model's own deterministic RNG.
//     p_ahead is tunable. New quantity joining the level goes behind me
//     (price-time priority: I was there first).
//
// Fill rules (both models):
//   - A trade executing AT my price consumes the queue from the front.
//     If trade_qty > qty_ahead, I am filled for
//     min(remaining, trade_qty - qty_ahead).
//   - A trade THROUGH my price (better than my price) fills my entire
//     remaining quantity (the level was swept).
//   - Partial fills are the norm; fills never exceed remaining.
//
// All quantities are int64 lots/ticks. p_ahead is a double in [0,1]
// (a model parameter, not a price).

#include <concepts>
#include <cstdint>

#include "kairos/latency.hpp"  // for splitmix64

namespace kairos {

enum class SimSide : std::uint8_t { Bid = 0, Ask = 1 };

// One of my resting limit orders tracked by the queue model.
struct TrackedOrder {
    std::uint64_t id = 0;
    SimSide side = SimSide::Bid;
    std::int64_t price_ticks = 0;
    std::int64_t qty_lots = 0;    // original size
    std::int64_t remaining = 0;   // unfilled
    std::int64_t qty_ahead = 0;   // lots ahead of me in the queue
    std::int64_t qty_behind = 0;  // lots behind me in the queue
    // The HISTORICAL level qty at the last update. My own fills do not
    // change the historical book, so deltas are computed against this,
    // not against qty_ahead + remaining + qty_behind (which drifts as I
    // get filled).
    std::int64_t last_level_qty = 0;
};

// A fill of one of my orders, as determined by the model.
struct SimFill {
    std::uint64_t order_id = 0;
    SimSide side = SimSide::Bid;
    std::int64_t price_ticks = 0;
    std::int64_t qty_lots = 0;
    bool is_maker = true;  // false if my aggressive order took liquidity
};

template <typename Q>
concept QueueModel = requires(Q& q, TrackedOrder& o, std::int64_t qty) {
    { q.on_place(o, qty) } -> std::same_as<void>;
    { q.on_level_update(o, qty) } -> std::same_as<void>;
    { q.on_trade(o, qty) } -> std::same_as<std::int64_t>;
    { q.on_trade_through(o) } -> std::same_as<std::int64_t>;
};

// ---------------------------------------------------------------------------
// Risk-averse: back of queue; only trades advance me.
// ---------------------------------------------------------------------------

struct RiskAverseQueue {
    void on_place(TrackedOrder& o, std::int64_t level_qty) noexcept {
        o.qty_ahead = level_qty > o.qty_lots ? level_qty - o.qty_lots : 0;
        o.qty_behind = 0;
        o.last_level_qty = level_qty;
    }

    void on_level_update(TrackedOrder& o, std::int64_t new_level_qty) noexcept {
        // Pessimistic: level reductions are ignored (assumed cancels behind
        // me — but I am at the back, so they cannot advance me). However,
        // the feed is authoritative: the level cannot hold more than it
        // holds. If new_level_qty < qty_ahead + remaining, the queue ahead
        // must have shrunk; clamp rather than contradict the feed.
        // Note: new_level_qty is the HISTORICAL level; my fills do not
        // affect it (see TrackedOrder::last_level_qty).
        const std::int64_t max_ahead =
            new_level_qty > o.remaining ? new_level_qty - o.remaining : 0;
        if (o.qty_ahead > max_ahead) {
            o.qty_ahead = max_ahead;
        }
        // Level growth joins behind me (I was there first).
        o.qty_behind = new_level_qty > o.qty_ahead + o.remaining
                           ? new_level_qty - o.qty_ahead - o.remaining
                           : 0;
        o.last_level_qty = new_level_qty;
    }

    // trade_qty: lots traded AT my price against my level (the taker
    // consumed from the front of the queue). Returns lots filled.
    std::int64_t on_trade(TrackedOrder& o, std::int64_t trade_qty) noexcept {
        if (o.remaining <= 0 || trade_qty <= 0) {
            return 0;
        }
        if (trade_qty <= o.qty_ahead) {
            o.qty_ahead -= trade_qty;
            return 0;
        }
        const std::int64_t fill = trade_qty - o.qty_ahead < o.remaining
                                      ? trade_qty - o.qty_ahead
                                      : o.remaining;
        o.qty_ahead = 0;
        o.remaining -= fill;
        return fill;
    }

    // A trade through my price swept the level: fill everything.
    std::int64_t on_trade_through(TrackedOrder& o) noexcept {
        const std::int64_t fill = o.remaining;
        o.qty_ahead = 0;
        o.qty_behind = 0;
        o.remaining = 0;
        return fill;
    }
};

static_assert(QueueModel<RiskAverseQueue>);

// ---------------------------------------------------------------------------
// Naive: front of queue; fills immediately on any trade at my price.
// This is the "fill at touch" baseline for realism-gap experiments —
// deliberately optimistic, to quantify how much realism costs.
// ---------------------------------------------------------------------------

struct NaiveQueue {
    void on_place(TrackedOrder& o, std::int64_t level_qty) noexcept {
        o.qty_ahead = 0;
        o.qty_behind = 0;
        o.last_level_qty = level_qty;
    }

    void on_level_update(TrackedOrder& o,
                         std::int64_t new_level_qty) noexcept {
        o.last_level_qty = new_level_qty;
    }

    // Any trade at my price fills me immediately (I'm first in line).
    std::int64_t on_trade(TrackedOrder& o,
                          std::int64_t trade_qty) noexcept {
        if (o.remaining <= 0 || trade_qty <= 0) {
            return 0;
        }
        const std::int64_t fill =
            trade_qty < o.remaining ? trade_qty : o.remaining;
        o.remaining -= fill;
        return fill;
    }

    std::int64_t on_trade_through(TrackedOrder& o) noexcept {
        const std::int64_t fill = o.remaining;
        o.remaining = 0;
        return fill;
    }
};

static_assert(QueueModel<NaiveQueue>);

// ---------------------------------------------------------------------------
// Probabilistic: level reductions split ahead/behind by Binomial(D, p).
// ---------------------------------------------------------------------------

class ProbabilisticQueue {
public:
    // p_ahead: probability that a cancelled lot was ahead of me.
    // seed: deterministic RNG seed (same seed -> same attribution).
    explicit ProbabilisticQueue(double p_ahead = 0.5,
                                std::uint64_t seed = 0x9e3779b97f4a7c15ULL)
        : p_ahead_(p_ahead), rng_(seed != 0 ? seed : 0x9e3779b97f4a7c15ULL) {}

    void on_place(TrackedOrder& o, std::int64_t level_qty) noexcept {
        // Same initial assumption as risk-averse: back of queue. The
        // probabilistic part governs how the queue EVOLVES.
        o.qty_ahead = level_qty > o.qty_lots ? level_qty - o.qty_lots : 0;
        o.qty_behind = 0;
        o.last_level_qty = level_qty;
    }

    void on_level_update(TrackedOrder& o, std::int64_t new_level_qty) noexcept {
        // Delta in the HISTORICAL level (my fills don't move it).
        const std::int64_t delta = new_level_qty - o.last_level_qty;
        o.last_level_qty = new_level_qty;
        if (delta < 0) {
            std::int64_t d = -delta;
            std::int64_t ahead_cancel = binomial(d);
            if (ahead_cancel > o.qty_ahead) {
                ahead_cancel = o.qty_ahead;
            }
            o.qty_ahead -= ahead_cancel;
            d -= ahead_cancel;
            const std::int64_t behind_cancel =
                d < o.qty_behind ? d : o.qty_behind;
            o.qty_behind -= behind_cancel;
            d -= behind_cancel;
            // Any unattributed remainder must have been ahead (the feed is
            // authoritative about the total).
            if (d > 0) {
                o.qty_ahead = o.qty_ahead > d ? o.qty_ahead - d : 0;
            }
        } else if (delta > 0) {
            // New liquidity joins behind me (price-time priority).
            o.qty_behind += delta;
        }
    }

    std::int64_t on_trade(TrackedOrder& o, std::int64_t trade_qty) noexcept {
        if (o.remaining <= 0 || trade_qty <= 0) {
            return 0;
        }
        if (trade_qty <= o.qty_ahead) {
            o.qty_ahead -= trade_qty;
            return 0;
        }
        const std::int64_t fill = trade_qty - o.qty_ahead < o.remaining
                                      ? trade_qty - o.qty_ahead
                                      : o.remaining;
        o.qty_ahead = 0;
        o.remaining -= fill;
        return fill;
    }

    std::int64_t on_trade_through(TrackedOrder& o) noexcept {
        const std::int64_t fill = o.remaining;
        o.qty_ahead = 0;
        o.qty_behind = 0;
        o.remaining = 0;
        return fill;
    }

private:
    double p_ahead_;
    std::uint64_t rng_;

    // Deterministic binomial: count successes in d trials via splitmix64.
    std::int64_t binomial(std::int64_t d) noexcept {
        if (p_ahead_ <= 0.0) {
            return 0;
        }
        if (p_ahead_ >= 1.0) {
            return d;
        }
        const std::uint64_t threshold =
            static_cast<std::uint64_t>(p_ahead_ * 18446744073709551616.0);
        std::int64_t k = 0;
        for (std::int64_t i = 0; i < d; ++i) {
            if (splitmix64(rng_) < threshold) {
                ++k;
            }
        }
        return k;
    }
};

static_assert(QueueModel<ProbabilisticQueue>);

}  // namespace kairos
