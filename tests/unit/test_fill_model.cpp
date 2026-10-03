// Tests for queue-position / fill models and the simulated account.

#include <gtest/gtest.h>

#include <cstdint>

#include "kairos/account.hpp"
#include "kairos/fill_model.hpp"

namespace kairos {
namespace {

TrackedOrder make_order(std::uint64_t id, SimSide side, std::int64_t price,
                        std::int64_t qty) {
    TrackedOrder o;
    o.id = id;
    o.side = side;
    o.price_ticks = price;
    o.qty_lots = qty;
    o.remaining = qty;
    return o;
}

// --- RiskAverseQueue ---

TEST(FillModel, RiskAverseBackOfQueue) {
    RiskAverseQueue q;
    auto o = make_order(1, SimSide::Bid, 100, 10);
    q.on_place(o, 50);  // level has 50, I add 10 -> 40 ahead
    EXPECT_EQ(o.qty_ahead, 40);
    EXPECT_EQ(o.qty_behind, 0);
}

TEST(FillModel, RiskAverseTradesAdvance) {
    RiskAverseQueue q;
    auto o = make_order(1, SimSide::Bid, 100, 10);
    q.on_place(o, 50);
    // Trade of 30: consumes 30 of the 40 ahead, no fill.
    EXPECT_EQ(q.on_trade(o, 30), 0);
    EXPECT_EQ(o.qty_ahead, 10);
    EXPECT_EQ(o.remaining, 10);
    // Trade of 15: 10 ahead consumed, 5 filled.
    EXPECT_EQ(q.on_trade(o, 15), 5);
    EXPECT_EQ(o.qty_ahead, 0);
    EXPECT_EQ(o.remaining, 5);
}

TEST(FillModel, RiskAverseIgnoresCancels) {
    RiskAverseQueue q;
    auto o = make_order(1, SimSide::Bid, 100, 10);
    q.on_place(o, 50);  // 40 ahead
    // Level drops to 30 without any trade: pessimistic, ahead unchanged...
    // but the feed is authoritative: 30 < 40 + 10, so clamp ahead to 20.
    q.on_level_update(o, 30);
    EXPECT_EQ(o.qty_ahead, 20);
    // Level grows to 60: new liquidity behind me.
    q.on_level_update(o, 60);
    EXPECT_EQ(o.qty_behind, 30);  // 60 - 20 - 10
}

TEST(FillModel, RiskAverseTradeThrough) {
    RiskAverseQueue q;
    auto o = make_order(1, SimSide::Ask, 100, 10);
    q.on_place(o, 50);
    EXPECT_EQ(q.on_trade_through(o), 10);
    EXPECT_EQ(o.remaining, 0);
}

TEST(FillModel, RiskAverseFillNeverExceedsRemaining) {
    RiskAverseQueue q;
    auto o = make_order(1, SimSide::Bid, 100, 10);
    q.on_place(o, 50);
    // Huge trade: fill capped at remaining.
    EXPECT_EQ(q.on_trade(o, 1000), 10);
    EXPECT_EQ(o.remaining, 0);
    EXPECT_EQ(q.on_trade(o, 100), 0);  // nothing left
}

// --- ProbabilisticQueue ---

TEST(FillModel, ProbabilisticDeterministic) {
    ProbabilisticQueue q1(0.5, 12345), q2(0.5, 12345);
    auto o1 = make_order(1, SimSide::Bid, 100, 10);
    auto o2 = make_order(1, SimSide::Bid, 100, 10);
    q1.on_place(o1, 100);
    q2.on_place(o2, 100);
    for (int i = 0; i < 20; ++i) {
        q1.on_level_update(o1, 90 - i);
        q2.on_level_update(o2, 90 - i);
        EXPECT_EQ(o1.qty_ahead, o2.qty_ahead);
        EXPECT_EQ(o1.qty_behind, o2.qty_behind);
    }
}

TEST(FillModel, ProbabilisticConservesTotal) {
    ProbabilisticQueue q(0.5, 999);
    auto o = make_order(1, SimSide::Bid, 100, 10);
    q.on_place(o, 100);
    // Shrink the level several times; ahead+remaining+behind must equal
    // the level qty (the feed is authoritative).
    std::int64_t level = 100;
    for (int i = 0; i < 10; ++i) {
        level -= 7;
        q.on_level_update(o, level);
        EXPECT_EQ(o.qty_ahead + o.remaining + o.qty_behind, level)
            << "iteration " << i;
    }
}

TEST(FillModel, ProbabilisticNewLiquidityBehind) {
    ProbabilisticQueue q(0.5, 1);
    auto o = make_order(1, SimSide::Bid, 100, 10);
    q.on_place(o, 50);  // 40 ahead
    q.on_level_update(o, 70);  // +20 joins behind
    EXPECT_EQ(o.qty_ahead, 40);
    EXPECT_EQ(o.qty_behind, 20);
}

TEST(FillModel, ProbabilisticP0IsPessimistic) {
    // p_ahead = 0: reductions never attributed ahead (except when forced
    // by the feed). Behaves like risk-averse for cancels.
    ProbabilisticQueue q(0.0, 1);
    auto o = make_order(1, SimSide::Bid, 100, 10);
    q.on_place(o, 50);
    q.on_level_update(o, 30);  // -20, all behind (but behind is 0) -> forced ahead
    EXPECT_EQ(o.qty_ahead, 20);  // clamped: 30 - 10 remaining
}

TEST(FillModel, ProbabilisticP1IsOptimistic) {
    // p_ahead = 1: reductions attributed ahead first.
    ProbabilisticQueue q(1.0, 1);
    auto o = make_order(1, SimSide::Bid, 100, 10);
    q.on_place(o, 50);  // 40 ahead
    // Grow behind first so there's something to attribute to.
    q.on_level_update(o, 70);  // behind = 20
    // Shrink by 10: all 10 attributed ahead (p=1).
    q.on_level_update(o, 60);
    EXPECT_EQ(o.qty_ahead, 30);
    EXPECT_EQ(o.qty_behind, 20);
}

// --- Account ---

TEST(Account, BuySellRoundTrip) {
    Account a;
    a.maker_fee_bp = 0;
    a.taker_fee_bp = 0;
    a.apply_fill(SimSide::Bid, 100, 10, true);   // buy 10 @ 100
    EXPECT_EQ(a.position_lots, 10);
    EXPECT_EQ(a.cash_lots, -1000);
    a.apply_fill(SimSide::Ask, 110, 10, true);   // sell 10 @ 110
    EXPECT_EQ(a.position_lots, 0);
    EXPECT_EQ(a.cash_lots, 100);                  // -1000 + 1100
    EXPECT_EQ(a.realized_pnl_lots, 100);          // 10 * (110 - 100)
}

TEST(Account, FeesAndRebate) {
    Account a;
    a.maker_fee_bp = -10;  // 10bp rebate
    a.taker_fee_bp = 30;   // 30bp fee
    a.apply_fill(SimSide::Bid, 10000, 100, true);  // maker buy, rebate
    // notional = 1e6, fee = 1e6 * -10 / 1e4 = -1000 (rebate)
    EXPECT_EQ(a.total_fees_lots, -1000);
    EXPECT_EQ(a.cash_lots, -1000000 + 1000);  // rebate reduces cost
    a.apply_fill(SimSide::Ask, 10000, 100, false);  // taker sell, fee
    // fee = 1e6 * 30 / 1e4 = 3000
    EXPECT_EQ(a.total_fees_lots, 2000);
}

TEST(Account, PartialCloseAndFlip) {
    Account a;
    a.apply_fill(SimSide::Bid, 100, 10, true);  // long 10 @ 100
    a.apply_fill(SimSide::Ask, 120, 4, true);   // sell 4 @ 120
    EXPECT_EQ(a.position_lots, 6);
    EXPECT_EQ(a.realized_pnl_lots, 80);  // 4 * (120 - 100)
    // Flip to short: sell 10 more (6 close, 4 open short @ 90)
    a.apply_fill(SimSide::Ask, 90, 10, true);
    EXPECT_EQ(a.position_lots, -4);
    EXPECT_EQ(a.realized_pnl_lots, 80 + 6 * (90 - 100));  // -60 + 80 = 20
    EXPECT_EQ(a.avg_entry_ticks, 90);
}

TEST(Account, UnrealizedPnl) {
    Account a;
    a.apply_fill(SimSide::Bid, 100, 10, true);
    EXPECT_EQ(a.unrealized_pnl_lots(110), 100);   // long gains
    EXPECT_EQ(a.unrealized_pnl_lots(90), -100);   // long loses
    Account s;
    s.apply_fill(SimSide::Ask, 100, 10, true);    // short 10 @ 100
    EXPECT_EQ(s.unrealized_pnl_lots(90), 100);    // short gains on drop
    EXPECT_EQ(s.unrealized_pnl_lots(110), -100);
}

TEST(Account, PnlIdentity) {
    // Starting from zero cash: equity == realized + unrealized - fees.
    Account a;
    a.taker_fee_bp = 10;
    a.apply_fill(SimSide::Bid, 100, 10, false);
    a.apply_fill(SimSide::Ask, 110, 6, false);
    const auto mark = 105;
    EXPECT_EQ(a.equity_lots(mark),
              a.realized_pnl_lots + a.unrealized_pnl_lots(mark) -
                  a.total_fees_lots);
}

}  // namespace
}  // namespace kairos
