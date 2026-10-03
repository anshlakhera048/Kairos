#pragma once

// Tournament harness for the Phase 4 arena (4B).
//
// A live, deterministic market: flow generators and ONE participant bot
// act on a shared Engine (the Phase 1 matching core), stepped forward in
// logical time. The bot has market impact — its orders rest in the book
// and get hit by flow.
//
// Each bot runs in its own Arena instance with the same seeds and flow:
// "same seeds for every bot" means identical market conditions, compared
// via the leaderboard. Bots do not interact with each other (cleaner
// fairness than a shared free-for-all).
//
// Fairness: every bot gets the same logical latency (feed + order), the
// same delayed book view, identical fees. The event loop is
// single-threaded and deterministic: same seed -> same outcome.
//
// Anti-cheat (enforced by the harness, not by trust):
//   - No future info: the bot sees only the delayed view, never the engine.
//   - Time budget: per-callback wall-clock limit; exceed -> disqualified.
//   - Position limit: hard cap on |position|; breaching orders are rejected.
//   - Order rate limit: max orders/sec; exceed -> rejected.

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "kairos/account.hpp"
#include "kairos/engine.hpp"
#include "kairos/flow.hpp"
#include "kairos/l2_book.hpp"
#include "kairos/latency.hpp"
#include "kairos/simulator.hpp"  // TradeInfo, SimFill, AckInfo

namespace kairos {
namespace arena {

// ---------------------------------------------------------------------------
// Scoring.
// ---------------------------------------------------------------------------

struct Score {
    double pnl = 0.0;
    double inventory_penalty = 0.0;
    double risk_adjusted = 0.0;
    double total = 0.0;

    // Why not raw PnL: a bot can warehouse directional risk and get lucky
    // on one seed. The inventory penalty charges for holding risk (it
    // costs spread to flatten); the risk term rewards consistency across
    // seeds.
};

struct ScoringConfig {
    double lambda_inventory = 1.0;
    double lambda_risk = 1.0;
    double spread_cost_per_lot = 0.01;
};

// ---------------------------------------------------------------------------
// Arena configuration.
// ---------------------------------------------------------------------------

struct ArenaConfig {
    std::uint64_t duration_ns = 0;
    std::uint64_t seed = 0;

    std::uint64_t feed_latency_ns = 5'000'000;
    std::uint64_t order_latency_ns = 2'000'000;

    std::int64_t maker_fee_bp = -10;
    std::int64_t taker_fee_bp = 30;

    std::int64_t max_position_lots = 10'000'000;
    std::uint64_t max_orders_per_sec = 100;
    std::uint64_t callback_time_budget_ns = 5'000'000;  // 5ms

    std::uint64_t timer_interval_ns = 500'000'000;

    ScoringConfig scoring;
};

// ---------------------------------------------------------------------------
// Per-bot result (one seed).
// ---------------------------------------------------------------------------

struct BotResult {
    std::string bot_name;
    std::uint64_t seed = 0;
    std::int64_t final_position_lots = 0;
    std::int64_t realized_pnl_lots = 0;
    std::uint64_t fills = 0;
    std::uint64_t acks = 0;
    std::uint64_t rejects = 0;
    bool disqualified = false;
    std::string dq_reason;

    std::vector<std::pair<std::uint64_t, std::int64_t>> equity_curve;
    std::vector<std::pair<std::uint64_t, std::int64_t>> inventory_curve;
};

// ---------------------------------------------------------------------------
// Arena: runs one bot against flow generators on a shared engine.
// ---------------------------------------------------------------------------

template <typename Bot>
class Arena {
public:
    // The bot must satisfy the Phase 3 Strategy concept (seven callbacks
    // taking Context&). The arena provides the Context.
    class Context {
    public:
        std::uint64_t now() const;
        const L2Book& book() const;  // delayed view
        const Account& account() const;

        std::uint64_t send_limit(SimSide side, std::int64_t price_ticks,
                                 std::int64_t qty_lots, bool post_only = true);
        void cancel(std::uint64_t order_id);
        std::uint64_t modify(std::uint64_t order_id,
                             std::int64_t new_price_ticks,
                             std::int64_t new_qty_lots);

    private:
        friend class Arena;
        explicit Context(Arena* arena) : arena_(arena) {}
        Arena* arena_;
    };

    explicit Arena(const ArenaConfig& cfg) : cfg_(cfg), engine_(make_engine_config()) {}

private:
    static OrderBookConfig make_engine_config() {
        OrderBookConfig ob_cfg;
        ob_cfg.base_price = Price(10000);
        ob_cfg.half_band = 2048;
        return ob_cfg;
    }

public:

    void add_generator(std::unique_ptr<FlowGenerator> gen) {
        generators_.push_back(std::move(gen));
    }

    BotResult run(Bot& bot, const std::string& bot_name);

    static Score score(const std::vector<BotResult>& results,
                       const ScoringConfig& cfg);

private:
    ArenaConfig cfg_;
    Engine engine_;
    std::vector<std::unique_ptr<FlowGenerator>> generators_;

    // Order tracking: order_id -> info (for fill detection).
    struct TrackedOrder {
        std::uint64_t id = 0;
        bool is_bot = false;
        bool is_bid = false;
        std::int64_t price_ticks = 0;
        std::int64_t qty_remaining = 0;
    };
    std::vector<TrackedOrder> tracked_;
    std::uint64_t next_order_id_ = 1;

    // Per-run state.
    L2Book view_;  // bot's delayed view
    Account account_;
    std::uint64_t strategy_now_ = 0;

    // Pending bot orders (with latency). Cleared at the start of run().
    struct PendingOrder {
        std::uint64_t effect_time;
        std::uint64_t id;
        SimSide side;
        std::int64_t price;
        std::int64_t qty;
        bool post_only;
    };
    std::vector<PendingOrder> pending_;
    std::vector<std::uint64_t> order_times_;  // for rate limiting
    std::uint64_t limit_rejects_ = 0;  // orders blocked by anti-cheat limits

    // Bot order submission (called by Context).
    std::uint64_t bot_send_limit(SimSide side, std::int64_t price_ticks,
                                 std::int64_t qty_lots, bool post_only);
    void bot_cancel(std::uint64_t order_id);
    std::uint64_t bot_modify(std::uint64_t order_id,
                             std::int64_t new_price_ticks,
                             std::int64_t new_qty_lots);
};

}  // namespace arena
}  // namespace kairos

#include "kairos/arena_impl.hpp"
