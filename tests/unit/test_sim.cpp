// End-to-end: replay the golden capture through a queue model and the
// simulated account. Places a resting bid at the best bid, tracks its
// queue position through diffs, fills it on trades, and books PnL.

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <vector>

#include "kairos/account.hpp"
#include "kairos/data_format.hpp"
#include "kairos/fill_model.hpp"
#include "kairos/l2_book.hpp"

namespace kairos {
namespace {

const char* kGolden = TEST_DATA_DIR "/golden.kai";

// Golden file recap (price_scale=100, qty_scale=1e8):
//   snapshot seq=1000: bids 99900x1e9, 99800x5e8; asks 100100x8e8, 100200x3e8
//   diff seq=1001: bid 99900 -> 7e8; ask 100300 += 2e8; bid 99800 removed
//   trade seq=1002: buyer-initiated (taker=bid) 100100 x 2.5e8
//   diff seq=1003: ask 100100 removed

TEST(Simulator, GoldenFileEndToEnd) {
    data::CaptureReader reader({kGolden});
    L2Book book;
    RiskAverseQueue queue;
    Account account;
    account.maker_fee_bp = -10;  // 10bp rebate

    TrackedOrder my_bid;
    bool placed = false;
    int fills = 0;

    data::EventView ev;
    while (reader.next(ev)) {
        book.apply(ev);
        const auto type = static_cast<data::EventType>(ev.header->type);
        if (type == data::EventType::Snapshot && !placed) {
            L2Level b;
            ASSERT_TRUE(book.best_bid(b));
            my_bid.id = 1;
            my_bid.side = SimSide::Bid;
            my_bid.price_ticks = b.price_ticks;  // join the best bid
            my_bid.qty_lots = 100000000LL;       // 1.0 lots
            my_bid.remaining = my_bid.qty_lots;
            queue.on_place(my_bid, b.qty_lots);
            placed = true;
        } else if (type == data::EventType::Diff && placed &&
                   my_bid.remaining > 0) {
            queue.on_level_update(my_bid,
                                  book.level_qty(true, my_bid.price_ticks));
        } else if (type == data::EventType::Trade && placed &&
                   my_bid.remaining > 0) {
            const data::LevelEntry& l = ev.levels[0];
            const bool taker_is_bid = (l.side == 0);
            std::int64_t filled = 0;
            if (l.price_ticks == my_bid.price_ticks && !taker_is_bid) {
                // Seller-initiated trade at my bid price: hits my level.
                filled = queue.on_trade(my_bid, l.qty_lots);
            } else if (l.price_ticks < my_bid.price_ticks) {
                // Traded through my bid.
                filled = queue.on_trade_through(my_bid);
            }
            if (filled > 0) {
                ++fills;
                account.apply_fill(SimSide::Bid, my_bid.price_ticks, filled,
                                   true);
            }
        }
        EXPECT_TRUE(book.check_invariants());
    }

    EXPECT_TRUE(placed);
    // My bid joined 99900. The only trade is buyer-initiated at 100100
    // (the ask side) — it does NOT touch my bid. So: no fills, still
    // resting, no PnL.
    EXPECT_EQ(fills, 0);
    EXPECT_EQ(my_bid.remaining, 100000000LL);
    EXPECT_EQ(account.position_lots, 0);
    // Queue state: placed at snapshot (1e9 at 99900 -> 9e8 ahead), then
    // diff reduced the level to 7e8 -> clamped ahead to 6e8.
    EXPECT_EQ(my_bid.qty_ahead, 600000000LL);
}

TEST(Simulator, MakerFillOnAggressiveSell) {
    // Place a bid, then drive a seller-initiated trade at my price through
    // the queue model directly.
    RiskAverseQueue queue;
    Account account;
    account.maker_fee_bp = -10;
    TrackedOrder o;
    o.id = 7;
    o.side = SimSide::Bid;
    o.price_ticks = 99900;
    o.qty_lots = 100000000LL;
    o.remaining = 100000000LL;
    queue.on_place(o, 1000000000LL);  // 9e8 ahead of me
    // Seller dumps 9.5e8 at my price: 9e8 ahead consumed, 0.5e8 fills me.
    const std::int64_t filled = queue.on_trade(o, 950000000LL);
    EXPECT_EQ(filled, 50000000LL);
    EXPECT_EQ(o.remaining, 50000000LL);
    account.apply_fill(SimSide::Bid, o.price_ticks, filled, true);
    EXPECT_EQ(account.position_lots, 50000000LL);
    // notional = 5e7 * 99900; fee = notional * -10bp.
    const std::int64_t notional = 50000000LL * 99900LL;
    const std::int64_t expected_fee = notional * -10 / 10000;
    EXPECT_EQ(account.total_fees_lots, expected_fee);
    EXPECT_EQ(account.cash_lots, -notional - expected_fee);
}

}  // namespace
}  // namespace kairos
