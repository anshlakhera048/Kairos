// Tests for the strategy API + simulator: basic flow, latency enforcement,
// and the no-lookahead cheat test.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "kairos/simulator.hpp"

namespace kairos {
namespace {

const char* kGolden = TEST_DATA_DIR "/golden.kai";

using Feed = ConstantLatency;
using Order = ConstantLatency;
using Q = RiskAverseQueue;

// A strategy that does nothing. Verifies the simulator runs end to end.
struct NullStrategy {
    using Ctx = Simulator<NullStrategy, Feed, Order, Q>::Context;
    void on_book_update(Ctx&) {}
    void on_trade(Ctx&, const TradeInfo&) {}
    void on_own_fill(Ctx&, const SimFill&) {}
    void on_ack(Ctx&, const AckInfo&) {}
    void on_reject(Ctx&, std::uint64_t) {}
    void on_cancel(Ctx&, std::uint64_t) {}
    void on_timer(Ctx&) {}
};

TEST(SimulatorApi, NullStrategyRuns) {
    typename Simulator<NullStrategy, Feed, Order, Q>::Config cfg;
    cfg.feed_lat = Feed{1000};
    cfg.order_lat = Order{2000};
    Simulator<NullStrategy, Feed, Order, Q> sim(cfg);
    NullStrategy s;
    const auto res = sim.run({kGolden}, s);
    EXPECT_EQ(res.market_events, 4u);
    EXPECT_EQ(res.fills, 0u);
}

// Sends one post-only bid at the best bid on the first book update, then
// cancels it on the first timer. Verifies ack/cancel flow and timing.
struct PingStrategy {
    using Ctx = Simulator<PingStrategy, Feed, Order, Q>::Context;
    std::uint64_t order_id = 0;
    std::uint64_t send_time = 0;
    std::uint64_t ack_effective = 0;
    int acks = 0;
    int cancels = 0;
    bool sent = false;

    void on_book_update(Ctx& ctx) {
        if (!sent) {
            L2Level b;
            if (ctx.book().best_bid(b)) {
                send_time = ctx.now();
                order_id = ctx.send_limit(SimSide::Bid, b.price_ticks,
                                          1000000LL, true);
                sent = true;
            }
        }
    }
    void on_trade(Ctx&, const TradeInfo&) {}
    void on_own_fill(Ctx&, const SimFill&) {}
    void on_ack(Ctx& ctx, const AckInfo& a) {
        ++acks;
        ack_effective = a.effective_exchange_ts;
        // Cancel it on the next timer instead; just record here.
        (void)ctx;
    }
    void on_reject(Ctx&, std::uint64_t) {}
    void on_cancel(Ctx&, std::uint64_t) { ++cancels; }
    void on_timer(Ctx& ctx) {
        if (sent && acks > 0 && cancels == 0) {
            ctx.cancel(order_id);
        }
    }
};

TEST(SimulatorApi, OrderLatencyEnforced) {
    typename Simulator<PingStrategy, Feed, Order, Q>::Config cfg;
    cfg.feed_lat = Feed{1000};
    cfg.order_lat = Order{5000};
    cfg.timer_interval_ns = 1000000;  // 1ms timers
    Simulator<PingStrategy, Feed, Order, Q> sim(cfg);
    PingStrategy s;
    const auto res = sim.run({kGolden}, s);
    EXPECT_EQ(s.acks, 1);
    // The order was sent at send_time (strategy time); it took effect at
    // the exchange at >= send_time + order latency.
    EXPECT_GE(s.ack_effective, s.send_time + 5000);
    EXPECT_EQ(res.acks, 1u);
    EXPECT_EQ(s.cancels, 1);
}

// --- The cheat test ---
//
// A strategy that tries to benefit from seeing the future. It records the
// lag between a trade's exchange timestamp and the time it becomes
// visible. If the simulator ever delivers a trade "early" (before its
// feed latency has elapsed), the strategy could front-run — the test
// fails.

struct SnoopingStrategy {
    using Ctx = Simulator<SnoopingStrategy, Feed, Order, Q>::Context;
    std::uint64_t feed_lat_ns = 0;
    std::uint64_t min_lag_ns = ~0ULL;
    int trades_seen = 0;

    void on_book_update(Ctx&) {}
    void on_trade(Ctx& ctx, const TradeInfo& t) {
        ++trades_seen;
        // now() is the delivery time; exchange_ts_ns is when it happened.
        // The difference must be at least the feed latency.
        const std::uint64_t lag = ctx.now() - t.exchange_ts_ns;
        if (lag < min_lag_ns) {
            min_lag_ns = lag;
        }
    }
    void on_own_fill(Ctx&, const SimFill&) {}
    void on_ack(Ctx&, const AckInfo&) {}
    void on_reject(Ctx&, std::uint64_t) {}
    void on_cancel(Ctx&, std::uint64_t) {}
    void on_timer(Ctx&) {}
};

TEST(SimulatorApi, NoLookaheadCheatFails) {
    typename Simulator<SnoopingStrategy, Feed, Order, Q>::Config cfg;
    cfg.feed_lat = Feed{10000};
    cfg.order_lat = Order{2000};
    Simulator<SnoopingStrategy, Feed, Order, Q> sim(cfg);
    SnoopingStrategy s;
    s.feed_lat_ns = 10000;
    const auto res = sim.run({kGolden}, s);
    EXPECT_GT(s.trades_seen, 0);
    // Every trade was delayed by at least the feed latency: the strategy
    // never saw the future.
    EXPECT_GE(s.min_lag_ns, s.feed_lat_ns);
    EXPECT_EQ(res.market_events, 4u);
}

// A strategy that tries to backdate: it sends an order and expects it to
// be effective immediately. It isn't — the ack proves the latency.
struct ImpatientStrategy {
    using Ctx = Simulator<ImpatientStrategy, Feed, Order, Q>::Context;
    std::uint64_t send_time = 0;
    std::uint64_t effective = 0;
    bool done = false;

    void on_book_update(Ctx& ctx) {
        if (!done) {
            L2Level b;
            if (ctx.book().best_bid(b)) {
                send_time = ctx.now();
                // Non-post-only, priced to cross: would fill immediately
                // if there were no order latency.
                L2Level a;
                if (ctx.book().best_ask(a)) {
                    ctx.send_limit(SimSide::Bid, a.price_ticks, 1000LL,
                                   false);
                    done = true;
                }
            }
        }
    }
    void on_trade(Ctx&, const TradeInfo&) {}
    void on_own_fill(Ctx&, const SimFill&) {}
    void on_ack(Ctx&, const AckInfo& ack) { effective = ack.effective_exchange_ts; }
    void on_reject(Ctx&, std::uint64_t) {}
    void on_cancel(Ctx&, std::uint64_t) {}
    void on_timer(Ctx&) {}
};

TEST(SimulatorApi, NoBackdateCheatFails) {
    typename Simulator<ImpatientStrategy, Feed, Order, Q>::Config cfg;
    cfg.feed_lat = Feed{1000};
    cfg.order_lat = Order{7000};
    Simulator<ImpatientStrategy, Feed, Order, Q> sim(cfg);
    ImpatientStrategy s;
    sim.run({kGolden}, s);
    EXPECT_TRUE(s.done);
    // Even though the order was marketable, it could not take effect
    // before send_time + order latency.
    EXPECT_GE(s.effective, s.send_time + 7000);
}

}  // namespace
}  // namespace kairos
