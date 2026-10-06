// Focused tests for ASStrategy requote gating.
//
// Verifies the strategy only cancels/re-places quotes when the desired
// quotes actually change, and that rejected/cancelled quotes are
// forgotten so the next requote re-places them.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "kairos/fill_model.hpp"  // SimSide
#include "kairos/l2_book.hpp"
#include "kairos/simulator.hpp"  // TradeInfo, AckInfo
#include "strategies/avellaneda_stoikov.hpp"

namespace kairos {
namespace {

// Minimal context satisfying what ASStrategy uses: now(), book(),
// account().position_lots, send_limit(), cancel().
struct FakeAccount {
    std::int64_t position_lots = 0;
};

struct FakeCtx {
    L2Book book_;
    FakeAccount account_;
    std::uint64_t now_ns = 1'000'000'000ULL;
    std::uint64_t next_id_ = 1;
    int sends = 0;
    bool block_sends = false;  // send_limit returns 0 (limit-blocked)
    std::vector<std::uint64_t> sent_ids;
    std::vector<std::uint64_t> cancelled_ids;
    struct SentOrder {
        SimSide side;
        std::int64_t price;
    };
    std::vector<SentOrder> sent_orders;

    std::uint64_t now() const { return now_ns; }
    L2Book& book() { return book_; }
    FakeAccount& account() { return account_; }

    std::uint64_t send_limit(SimSide side, std::int64_t price,
                             std::int64_t, bool) {
        ++sends;
        if (block_sends) {
            return 0;  // blocked by rate/position limit; no callback
        }
        sent_ids.push_back(next_id_);
        sent_orders.push_back({side, price});
        return next_id_++;
    }
    void cancel(std::uint64_t id) { cancelled_ids.push_back(id); }
};

void set_touch(FakeCtx& ctx, std::int64_t bid, std::int64_t ask) {
    ctx.book_.clear();
    ctx.book_.upsert(true, bid, 1'000'000);
    ctx.book_.upsert(false, ask, 1'000'000);
}

TEST(ASRequoteGating, UnchangedBookSuppressesRequote) {
    ASStrategy strat(ASStrategyConfig{});
    FakeCtx ctx;
    set_touch(ctx, 10000, 10002);

    strat.on_book_update(ctx);
    EXPECT_EQ(ctx.sends, 2);  // bid + ask placed
    EXPECT_TRUE(ctx.cancelled_ids.empty());

    const int sends_after_first = ctx.sends;
    strat.on_book_update(ctx);  // same book, same inventory
    EXPECT_EQ(ctx.sends, sends_after_first);  // no churn
    EXPECT_TRUE(ctx.cancelled_ids.empty());
}

TEST(ASRequoteGating, PriceChangeReplacesQuotes) {
    ASStrategy strat(ASStrategyConfig{});
    FakeCtx ctx;
    set_touch(ctx, 10000, 10002);

    strat.on_book_update(ctx);
    ASSERT_EQ(ctx.sends, 2);
    const std::uint64_t old_bid = ctx.sent_ids[0];
    const std::uint64_t old_ask = ctx.sent_ids[1];

    set_touch(ctx, 10010, 10012);  // mid moved
    strat.on_book_update(ctx);
    EXPECT_EQ(ctx.sends, 4);  // both sides re-placed
    ASSERT_EQ(ctx.cancelled_ids.size(), 2u);
    EXPECT_EQ(ctx.cancelled_ids[0], old_bid);
    EXPECT_EQ(ctx.cancelled_ids[1], old_ask);
}

TEST(ASRequoteGating, RejectRecoveryReplaces) {
    ASStrategy strat(ASStrategyConfig{});
    FakeCtx ctx;
    set_touch(ctx, 10000, 10002);

    strat.on_book_update(ctx);
    ASSERT_EQ(ctx.sends, 2);
    const std::uint64_t bid_id = ctx.sent_ids[0];
    const std::uint64_t ask_id = ctx.sent_ids[1];

    // Exchange rejects the bid quote: strategy must forget it so the
    // next requote re-places it instead of assuming it is live. The
    // requote pulls the surviving ask and re-places both sides.
    strat.on_reject(ctx, bid_id);
    strat.on_book_update(ctx);  // same book, same desired prices
    EXPECT_EQ(ctx.sends, 4);    // bid re-placed, ask re-placed
    ASSERT_EQ(ctx.cancelled_ids.size(), 1u);
    EXPECT_EQ(ctx.cancelled_ids[0], ask_id);
}

TEST(ASRequoteGating, InventoryLimitQuotesReducingSideOnly) {
    ASStrategyConfig cfg;
    cfg.max_inventory_lots = 10'000'000;
    ASStrategy strat(cfg);
    FakeCtx ctx;
    set_touch(ctx, 10000, 10002);
    ctx.account_.position_lots = cfg.max_inventory_lots;  // too long

    strat.on_book_update(ctx);
    EXPECT_EQ(ctx.sends, 1);  // ask only (reduces inventory)

    strat.on_book_update(ctx);  // unchanged: still gated
    EXPECT_EQ(ctx.sends, 1);
    EXPECT_TRUE(ctx.cancelled_ids.empty());

    // Inventory drops below the limit: bid side rejoins.
    ctx.account_.position_lots = 0;
    strat.on_book_update(ctx);
    EXPECT_EQ(ctx.sends, 3);  // ask re-placed + bid placed
}

TEST(ASRequoteGating, InventoryUnitsStayNearTouch) {
    // Regression test: the model takes inventory in whole lots while the
    // engine tracks micro-lots (qty_scale = 1e6). Passing micro-lots
    // directly made the skew 1e6x too strong: one fill threw quotes ~400k
    // ticks from the touch. With the conversion, a 1-lot position skews
    // quotes by only a few ticks.
    ASStrategyConfig cfg;
    cfg.model.gamma = 1500.0;
    cfg.model.sigma = 0.0002;
    cfg.model.kappa = 2.0;
    cfg.model.horizon_s = 3600.0;

    for (std::int64_t inv : {0, 1'000'000, -1'000'000, 5'000'000}) {
        // Fresh strategy per inventory level (bypasses requote gating).
        ASStrategy s(cfg);
        FakeCtx c;
        set_touch(c, 10000, 10002);
        c.account_.position_lots = inv;
        s.on_book_update(c);
        ASSERT_EQ(c.sent_orders.size(), 2u) << "inv=" << inv;
        for (const auto& o : c.sent_orders) {
            // Quotes must be within a few ticks of the touch, not
            // hundreds of thousands of ticks away.
            EXPECT_GE(o.price, 10000 - 100) << "inv=" << inv;
            EXPECT_LE(o.price, 10002 + 100) << "inv=" << inv;
        }
    }
}

TEST(ASRequoteGating, TimerAloneDoesNotChurn) {
    ASStrategy strat(ASStrategyConfig{});
    FakeCtx ctx;
    set_touch(ctx, 10000, 10002);

    strat.on_book_update(ctx);
    ASSERT_EQ(ctx.sends, 2);
    // Repeated timer ticks with a static book must not churn orders.
    for (int i = 0; i < 10; ++i) {
        strat.on_timer(ctx);
    }
    EXPECT_EQ(ctx.sends, 2);
    EXPECT_TRUE(ctx.cancelled_ids.empty());
}

TEST(ASRequoteGating, BlockedSendBacksOff) {
    ASStrategy strat(ASStrategyConfig{});
    FakeCtx ctx;
    ctx.block_sends = true;  // every send returns 0 (limit-blocked)
    set_touch(ctx, 10000, 10002);

    strat.on_book_update(ctx);
    EXPECT_EQ(ctx.sends, 2);  // both sides attempted once

    // Book updates with unchanged quotes must not hammer the limit:
    // no retry until the backoff interval elapses.
    for (int i = 0; i < 100; ++i) {
        strat.on_book_update(ctx);
    }
    EXPECT_EQ(ctx.sends, 2);

    // After the backoff interval, the retry re-attempts the sends.
    ctx.now_ns += 600'000'000ULL;
    strat.on_timer(ctx);
    EXPECT_EQ(ctx.sends, 4);

    // Still blocked: back off again, no spin.
    for (int i = 0; i < 100; ++i) {
        strat.on_book_update(ctx);
    }
    EXPECT_EQ(ctx.sends, 4);
}

}  // namespace
}  // namespace kairos
