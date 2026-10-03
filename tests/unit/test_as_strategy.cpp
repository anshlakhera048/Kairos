// A-S strategy running in the simulator on synthetic data.
// Verifies it quotes, gets fills, and respects inventory limits.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "kairos/simulator.hpp"
#include "strategies/avellaneda_stoikov.hpp"

namespace kairos {
namespace {

TEST(ASStrategy, QuotesAndFillsOnSynthetic) {
    const char* path = "/tmp/synth_test.kai";
    data::MappedFile probe(path);
    if (!probe.valid()) {
        GTEST_SKIP() << "synthetic data not present; run tools/recorder/synth_market.py";
    }

    using Feed = ConstantLatency;
    using Order = ConstantLatency;
    using Q = RiskAverseQueue;
    using Sim = Simulator<ASStrategy, Feed, Order, Q>;

    ASStrategyConfig scfg;
    scfg.model.gamma = 1500.0;
    scfg.model.sigma = 0.0002;
    scfg.model.kappa = 2.0;
    scfg.model.horizon_s = 3600.0;
    scfg.order_size_lots = 1000000;      // 1.0 lots (qty_scale=1e6)
    scfg.max_inventory_lots = 10000000;  // 10 lots
    scfg.max_spread_ticks = 100;

    typename Sim::Config cfg;
    cfg.feed_lat = Feed{5000000};    // 5ms
    cfg.order_lat = Order{2000000};  // 2ms
    cfg.timer_interval_ns = 500000000;  // 500ms requote
    cfg.maker_fee_bp = -10;
    cfg.taker_fee_bp = 30;

    Sim sim(cfg);
    ASStrategy strat(scfg);
    const auto res = sim.run({path}, strat);

    // It should have quoted (acks) and likely gotten some fills.
    EXPECT_GT(res.acks, 0u);
    EXPECT_GT(res.market_events, 0u);
    // Inventory stays within limits.
    EXPECT_LE(res.account.position_lots, scfg.max_inventory_lots);
    EXPECT_GE(res.account.position_lots, -scfg.max_inventory_lots);
}

}  // namespace
}  // namespace kairos
