#pragma once

// Flow generators for the Phase 4 arena (4A).
//
// Deterministic given a seed: each generator owns an independent RNG
// stream (splitmix64), so the flow is reproducible and generators do not
// interfere with each other's sequences.
//
// All generators act on the shared Engine (the Phase 1 matching core).
// The arena steps logical time forward and asks each generator when it
// next wants to act; generators submit orders/cancels directly.
//
// Model assumptions are documented per generator.

#include <cstdint>
#include <cmath>

#include "kairos/engine.hpp"
#include "kairos/order_book.hpp"

namespace kairos {
namespace arena {

// Deterministic 64-bit RNG (splitmix64). Same seed -> same sequence,
// across platforms.
class Rng {
public:
    explicit Rng(std::uint64_t seed) : state_(seed != 0 ? seed : 0x9e3779b97f4a7c15ULL) {}

    std::uint64_t next_u64() {
        std::uint64_t z = (state_ += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }

    // Uniform in [0, 1).
    double next_unit() {
        return (next_u64() >> 11) * (1.0 / 9007199254740992.0);
    }

    // Exponential with rate lambda (mean 1/lambda).
    double next_exp(double lambda) {
        return -std::log(1.0 - next_unit()) / lambda;
    }

    // Standard normal via Box-Muller.
    double next_gauss() {
        const double u1 = next_unit();
        const double u2 = next_unit();
        return std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }

private:
    std::uint64_t state_;
};

// ---------------------------------------------------------------------------
// Generator interface.
// ---------------------------------------------------------------------------

// A flow generator is asked when it next acts, then acts on the engine.
// The arena owns the event buffer; generators write events into it.
class FlowGenerator {
public:
    virtual ~FlowGenerator() = default;
    // Next action time in ns (UINT64_MAX = never).
    virtual std::uint64_t next_time() const = 0;
    // Perform the action at time now; returns the next action time.
    virtual std::uint64_t act(Engine& engine, std::uint64_t now, Event* out,
                              std::size_t out_capacity) = 0;
};

// ---------------------------------------------------------------------------
// NoiseTrader: uninformed Poisson flow.
// ---------------------------------------------------------------------------
//
// Assumptions: arrivals are memoryless (Poisson); each arrival is a market
// order with probability p_market (takes liquidity) or a limit order
// (provides liquidity at an exponential distance from the touch). Limit
// orders have an exponential lifetime (they cancel if not filled).
// Sizes are lognormal-ish (exp of normal).
//
// This is the "dumb" flow that pays the spread. It does not react to
// price — that is what makes it noise.

struct NoiseConfig {
    double arrival_rate_per_s = 2.0;  // Poisson rate
    double p_market = 0.4;            // P(arrival is a market order)
    double mean_size_lots = 1'000'000.0;
    double size_sigma = 0.8;          // lognormal shape
    double price_distance_ticks = 3.0;  // mean distance from touch (exp)
    double cancel_rate_per_s = 0.5;   // limit order lifetime
    std::uint64_t seed = 0;
};

class NoiseTrader : public FlowGenerator {
public:
    explicit NoiseTrader(const NoiseConfig& cfg, std::uint32_t owner);

    std::uint64_t next_time() const override { return next_time_; }
    std::uint64_t act(Engine& engine, std::uint64_t now, Event* out,
                      std::size_t out_capacity) override;

private:
    NoiseConfig cfg_;
    std::uint32_t owner_;
    Rng rng_;
    std::uint64_t next_time_ = 0;
    std::uint64_t order_seq_ = 0;
};

// ---------------------------------------------------------------------------
// InformedTrader: trades on a latent fair value.
// ---------------------------------------------------------------------------
//
// Assumptions (Glosten-Milgrom / Kyle spirit): there is a latent fair
// value v_t following a random walk with occasional jumps. The informed
// trader observes v_t; the market does not. When |v_t - mid_t| exceeds a
// threshold, the trader sends market orders toward v_t. The market learns
// from this flow (the book moves), which is what creates adverse
// selection for market makers: if you fill the informed trader, the
// price moves against you.
//
// The jump component models news arrivals. Between jumps, v_t is a
// Brownian motion with volatility sigma_v.

struct InformedConfig {
    // Fair value volatility in ticks per sqrt(second). E.g., 0.5 means the
    // fair value moves ~0.5 ticks per sqrt(s) on average.
    double sigma_v_ticks_per_sqrt_s = 0.5;
    double jump_rate_per_s = 0.002;      // news arrival rate
    double jump_sigma_ticks = 20.0;     // jump size in ticks
    double trade_threshold_ticks = 2.0;  // |v - mid| to trigger
    double trade_rate_per_s = 5.0;      // max trading rate when active
    std::int64_t trade_size_lots = 2'000'000;
    std::uint64_t seed = 0;
};

class InformedTrader : public FlowGenerator {
public:
    explicit InformedTrader(const InformedConfig& cfg, std::int64_t init_mid_ticks,
                            std::uint32_t owner);

    std::uint64_t next_time() const override { return next_time_; }
    std::uint64_t act(Engine& engine, std::uint64_t now, Event* out,
                      std::size_t out_capacity) override;

    // For testing: the current latent fair value.
    double fair_value() const { return fair_; }

private:
    InformedConfig cfg_;
    std::uint32_t owner_;
    Rng rng_;
    double fair_;  // latent fair value in ticks (float for the random walk)
    std::uint64_t next_time_ = 0;
    std::uint64_t order_seq_ = 0;
    std::uint64_t last_time_ = 0;
};

// ---------------------------------------------------------------------------
// FixedSpreadMaker: background liquidity provider.
// ---------------------------------------------------------------------------
//
// Quotes both sides at a fixed spread around the mid, refreshing on a
// timer. This is the "dumb" market maker — it does not skew for
// inventory and does not detect informed flow. It exists so the book
// always has depth for others to trade against.

struct FixedSpreadConfig {
    std::int64_t spread_ticks = 2;
    std::int64_t size_lots = 5'000'000;
    std::uint64_t refresh_interval_ns = 100'000'000;  // 100ms
    std::uint64_t seed = 0;
};

class FixedSpreadMaker : public FlowGenerator {
public:
    explicit FixedSpreadMaker(const FixedSpreadConfig& cfg, std::uint32_t owner);

    std::uint64_t next_time() const override { return next_time_; }
    std::uint64_t act(Engine& engine, std::uint64_t now, Event* out,
                      std::size_t out_capacity) override;

private:
    FixedSpreadConfig cfg_;
    std::uint32_t owner_;
    Rng rng_;
    std::uint64_t next_time_ = 0;
    std::uint64_t order_seq_ = 0;
    std::uint64_t bid_id_ = 0;
    std::uint64_t ask_id_ = 0;
};

// ---------------------------------------------------------------------------
// MomentumTrader: trend follower.
// ---------------------------------------------------------------------------
//
// Tracks an exponentially-weighted price trend. Buys when the trend is
// positive, sells when negative, via market orders. This creates
// short-term autocorrelation in order flow (real markets have this).

struct MomentumConfig {
    double trend_halflife_s = 5.0;     // EWMA halflife
    double trade_threshold = 0.5;      // |trend| in ticks to trigger
    double trade_rate_per_s = 2.0;
    std::int64_t trade_size_lots = 1'000'000;
    std::uint64_t seed = 0;
};

class MomentumTrader : public FlowGenerator {
public:
    explicit MomentumTrader(const MomentumConfig& cfg, std::uint32_t owner);

    std::uint64_t next_time() const override { return next_time_; }
    std::uint64_t act(Engine& engine, std::uint64_t now, Event* out,
                      std::size_t out_capacity) override;

private:
    MomentumConfig cfg_;
    std::uint32_t owner_;
    Rng rng_;
    double trend_ = 0.0;
    std::int64_t last_mid_ = 0;
    std::uint64_t last_time_ = 0;
    std::uint64_t next_time_ = 0;
    std::uint64_t order_seq_ = 0;
};

// ---------------------------------------------------------------------------
// Regimes: parameter presets to stress strategies.
// ---------------------------------------------------------------------------

struct Regime {
    const char* name;
    NoiseConfig noise;
    InformedConfig informed;
    // Background makers are always on; regime changes their intensity
    // via the noise/informed params.
};

// Calm: low volatility, little informed flow. Tests basic spread capture.
inline Regime regime_calm(std::uint64_t seed) {
    Regime r;
    r.name = "calm";
    r.noise.arrival_rate_per_s = 1.0;
    r.noise.seed = seed + 1;
    r.informed.sigma_v_ticks_per_sqrt_s = 0.5;
    r.informed.jump_rate_per_s = 0.0005;
    r.informed.jump_sigma_ticks = 10.0;
    r.informed.seed = seed + 2;
    return r;
}

// Volatile: high noise volatility, wider spreads. Tests inventory control.
inline Regime regime_volatile(std::uint64_t seed) {
    Regime r;
    r.name = "volatile";
    r.noise.arrival_rate_per_s = 4.0;
    r.noise.price_distance_ticks = 8.0;
    r.noise.seed = seed + 1;
    r.informed.sigma_v_ticks_per_sqrt_s = 1.5;
    r.informed.jump_rate_per_s = 0.005;
    r.informed.jump_sigma_ticks = 30.0;
    r.informed.seed = seed + 2;
    return r;
}

// Informed-heavy: aggressive informed trader. Tests adverse selection handling.
inline Regime regime_informed_heavy(std::uint64_t seed) {
    Regime r;
    r.name = "informed_heavy";
    r.noise.arrival_rate_per_s = 1.5;
    r.noise.seed = seed + 1;
    r.informed.sigma_v_ticks_per_sqrt_s = 1.0;
    r.informed.jump_rate_per_s = 0.01;
    r.informed.jump_sigma_ticks = 20.0;
    r.informed.trade_threshold_ticks = 1.0;
    r.informed.seed = seed + 2;
    return r;
}

}  // namespace arena
}  // namespace kairos
