// Unit tests for the OrderBook: every order type, edge cases, and the event
// stream contract. Invariants are checked after every operation.

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "kairos/error.hpp"
#include "kairos/events.hpp"
#include "kairos/order_book.hpp"
#include "kairos/types.hpp"

namespace kairos {
namespace {

OrderBookConfig test_config() {
    OrderBookConfig c;
    c.base_price = Price{1000};
    c.half_band = 16;
    c.max_orders = 1024;
    return c;
}

struct EventView {
    EventType type;
    std::uint64_t order_id;
    Price price;
    std::int64_t qty;
};

EventView view(const Event& e) {
    return {e.type, e.order_id.id, e.price, e.qty.lots};
}

class BookTest : public ::testing::Test {
protected:
    OrderBook book{test_config()};
    Event ev[64];

    OrderBook::OpResult add(std::uint64_t id, Side side, OrderType type, std::int64_t price,
                 std::int64_t qty, std::uint64_t ts = 1, std::uint32_t owner = 0) {
        auto r = book.add(OrderId{id}, side, type, Price{price}, Quantity{qty},
                          Timestamp{ts}, owner, ev, 64);
        EXPECT_TRUE(book.debug_check_invariants());
        return r;
    }

    OrderBook::OpResult cancel(std::uint64_t id, std::uint64_t ts = 1) {
        auto r = book.cancel(OrderId{id}, Timestamp{ts}, ev, 64);
        EXPECT_TRUE(book.debug_check_invariants());
        return r;
    }

    OrderBook::OpResult modify(std::uint64_t id, std::int64_t price, std::int64_t qty,
                    std::uint64_t ts = 1) {
        auto r = book.modify(OrderId{id}, Price{price}, Quantity{qty},
                             Timestamp{ts}, ev, 64);
        EXPECT_TRUE(book.debug_check_invariants());
        return r;
    }
};

TEST_F(BookTest, AddRestingLimitEmitsAck) {
    auto r = add(1, Side::Bid, OrderType::Limit, 999, 10);
    EXPECT_EQ(r.error, Error::Ok);
    ASSERT_EQ(r.events, 1u);
    auto v = view(ev[0]);
    EXPECT_EQ(v.type, EventType::Ack);
    EXPECT_EQ(v.order_id, 1u);
    EXPECT_EQ(v.price.ticks, 999);
    EXPECT_EQ(v.qty, 10);
    EXPECT_EQ(book.resting_count(), 1u);
    Price b{0};
    EXPECT_TRUE(book.best_bid(b));
    EXPECT_EQ(b.ticks, 999);
}

TEST_F(BookTest, EmptyBookQueries) {
    Price p{0};
    EXPECT_FALSE(book.best_bid(p));
    EXPECT_FALSE(book.best_ask(p));
    auto r = cancel(99);
    EXPECT_EQ(r.error, Error::UnknownId);
    EXPECT_EQ(r.events, 0u);
}

TEST_F(BookTest, CrossingLimitSweepsLevelsWithPriceTimePriority) {
    add(1, Side::Ask, OrderType::Limit, 1001, 5, 1);
    add(2, Side::Ask, OrderType::Limit, 1002, 5, 2);
    add(3, Side::Ask, OrderType::Limit, 1002, 5, 3);  // behind id 2

    auto r = add(4, Side::Bid, OrderType::Limit, 1003, 12, 4);
    EXPECT_EQ(r.error, Error::Ok);
    ASSERT_EQ(r.events, 4u);
    // Price priority first (1001 before 1002), then time priority (id 2 before id 3).
    EXPECT_EQ(view(ev[0]).type, EventType::Fill);
    EXPECT_EQ(view(ev[0]).order_id, 4u);
    EXPECT_EQ(view(ev[0]).price.ticks, 1001);
    EXPECT_EQ(view(ev[0]).qty, 5);
    EXPECT_EQ(view(ev[1]).price.ticks, 1002);
    EXPECT_EQ(view(ev[1]).qty, 5);
    EXPECT_EQ(view(ev[2]).price.ticks, 1002);
    EXPECT_EQ(view(ev[2]).qty, 2);
    EXPECT_EQ(view(ev[3]).type, EventType::Ack);
    EXPECT_EQ(view(ev[3]).qty, 0);  // fully filled

    Price a{0};
    EXPECT_TRUE(book.best_ask(a));
    EXPECT_EQ(a.ticks, 1002);  // id 3 has 3 remaining
    EXPECT_EQ(book.resting_count(), 1u);
}

TEST_F(BookTest, MarketOrderDiscardsRemainder) {
    add(1, Side::Ask, OrderType::Limit, 1001, 5, 1);
    auto r = add(2, Side::Bid, OrderType::Market, 0, 12, 2);
    EXPECT_EQ(r.error, Error::Ok);
    ASSERT_EQ(r.events, 2u);
    EXPECT_EQ(view(ev[0]).type, EventType::Fill);
    EXPECT_EQ(view(ev[0]).qty, 5);
    EXPECT_EQ(view(ev[1]).type, EventType::Ack);
    EXPECT_EQ(view(ev[1]).qty, 0);  // remainder discarded, not rested
    EXPECT_EQ(book.resting_count(), 0u);
}

TEST_F(BookTest, MarketOrderOnEmptyBook) {
    auto r = add(1, Side::Bid, OrderType::Market, 0, 10, 1);
    EXPECT_EQ(r.error, Error::Ok);
    ASSERT_EQ(r.events, 1u);
    EXPECT_EQ(view(ev[0]).type, EventType::Ack);
    EXPECT_EQ(view(ev[0]).qty, 0);
}

TEST_F(BookTest, IocFillsAndDiscardsRemainder) {
    add(1, Side::Ask, OrderType::Limit, 1001, 5, 1);
    auto r = add(2, Side::Bid, OrderType::Ioc, 1005, 12, 2);
    EXPECT_EQ(r.error, Error::Ok);
    ASSERT_EQ(r.events, 2u);
    EXPECT_EQ(view(ev[0]).type, EventType::Fill);
    EXPECT_EQ(view(ev[0]).qty, 5);
    EXPECT_EQ(view(ev[1]).type, EventType::Ack);
    EXPECT_EQ(view(ev[1]).qty, 0);
    EXPECT_EQ(book.resting_count(), 0u);  // nothing rested
}

TEST_F(BookTest, FokFillsCompletelyOrRejects) {
    add(1, Side::Ask, OrderType::Limit, 1001, 5, 1);
    add(2, Side::Ask, OrderType::Limit, 1002, 5, 2);

    auto ok = add(3, Side::Bid, OrderType::Fok, 1002, 10, 3);
    EXPECT_EQ(ok.error, Error::Ok);
    EXPECT_EQ(ok.events, 3u);  // 2 fills + Ack{0}
    EXPECT_EQ(book.resting_count(), 0u);

    add(4, Side::Ask, OrderType::Limit, 1001, 5, 4);
    auto bad = add(5, Side::Bid, OrderType::Fok, 1002, 6, 5);
    EXPECT_EQ(bad.error, Error::InsufficientQtyFok);
    ASSERT_EQ(bad.events, 1u);
    EXPECT_EQ(view(ev[0]).type, EventType::Reject);
    EXPECT_EQ(book.resting_count(), 1u);  // book untouched
}

TEST_F(BookTest, PostOnlyRestsOrRejects) {
    add(1, Side::Ask, OrderType::Limit, 1001, 5, 1);

    auto ok = add(2, Side::Bid, OrderType::PostOnly, 1000, 10, 2);
    EXPECT_EQ(ok.error, Error::Ok);
    EXPECT_EQ(view(ev[0]).type, EventType::Ack);
    EXPECT_EQ(view(ev[0]).qty, 10);
    EXPECT_EQ(book.resting_count(), 2u);

    auto bad = add(3, Side::Bid, OrderType::PostOnly, 1001, 10, 3);
    EXPECT_EQ(bad.error, Error::WouldCrossPostOnly);
    ASSERT_EQ(bad.events, 1u);
    EXPECT_EQ(view(ev[0]).type, EventType::Reject);
    EXPECT_EQ(book.resting_count(), 2u);  // untouched
}

TEST_F(BookTest, CancelRemovesAndEmits) {
    add(1, Side::Bid, OrderType::Limit, 999, 10, 1);
    auto r = cancel(1, 2);
    EXPECT_EQ(r.error, Error::Ok);
    ASSERT_EQ(r.events, 1u);
    EXPECT_EQ(view(ev[0]).type, EventType::Cancel);
    EXPECT_EQ(view(ev[0]).qty, 10);
    EXPECT_EQ(book.resting_count(), 0u);
    Price b{0};
    EXPECT_FALSE(book.best_bid(b));

    auto again = cancel(1, 3);
    EXPECT_EQ(again.error, Error::UnknownId);
}

TEST_F(BookTest, ModifyReduceKeepsPriority) {
    add(1, Side::Bid, OrderType::Limit, 999, 10, 1);
    add(2, Side::Bid, OrderType::Limit, 999, 10, 2);  // behind id 1

    auto r = modify(1, 999, 4, 3);
    EXPECT_EQ(r.error, Error::Ok);
    ASSERT_EQ(r.events, 1u);
    EXPECT_EQ(view(ev[0]).type, EventType::Modify);
    EXPECT_EQ(view(ev[0]).qty, 4);

    // id 1 must still be ahead of id 2: sweep 12 -> fills 4 (id 1), then 8 (id 2).
    auto s = add(3, Side::Ask, OrderType::Market, 0, 12, 4);
    EXPECT_EQ(s.error, Error::Ok);
    ASSERT_EQ(s.events, 3u);
    EXPECT_EQ(view(ev[0]).qty, 4);
    EXPECT_EQ(view(ev[1]).qty, 8);
}

TEST_F(BookTest, ModifyPriceChangeLosesPriority) {
    add(1, Side::Bid, OrderType::Limit, 999, 10, 1);
    add(2, Side::Bid, OrderType::Limit, 1000, 10, 2);  // ahead at 1000

    auto r = modify(1, 1000, 10, 3);
    EXPECT_EQ(r.error, Error::Ok);
    ASSERT_EQ(r.events, 2u);
    EXPECT_EQ(view(ev[0]).type, EventType::Cancel);  // old state gone
    EXPECT_EQ(view(ev[1]).type, EventType::Ack);    // re-added

    // id 2 is still ahead of id 1 at 1000: sweep 12 -> 10 (id 2), 2 (id 1).
    auto s = add(3, Side::Ask, OrderType::Market, 0, 12, 4);
    ASSERT_EQ(s.events, 3u);
    EXPECT_EQ(view(ev[0]).qty, 10);
    EXPECT_EQ(view(ev[1]).qty, 2);
}

TEST_F(BookTest, ModifyIncreaseLosesPriority) {
    add(1, Side::Bid, OrderType::Limit, 999, 10, 1);
    add(2, Side::Bid, OrderType::Limit, 999, 10, 2);

    auto r = modify(1, 999, 15, 3);  // size increase -> cancel/replace
    EXPECT_EQ(r.error, Error::Ok);
    EXPECT_EQ(view(ev[0]).type, EventType::Cancel);
    EXPECT_EQ(view(ev[1]).type, EventType::Ack);

    auto s = add(3, Side::Ask, OrderType::Market, 0, 12, 4);
    ASSERT_EQ(s.events, 3u);
    EXPECT_EQ(view(ev[0]).qty, 10);  // id 2 first now
    EXPECT_EQ(view(ev[1]).qty, 2);   // then id 1
}

TEST_F(BookTest, ModifyUnknownId) {
    auto r = modify(42, 999, 10, 1);
    EXPECT_EQ(r.error, Error::UnknownId);
}

TEST_F(BookTest, DuplicateIdRejected) {
    add(1, Side::Bid, OrderType::Limit, 999, 10, 1);
    auto r = add(1, Side::Ask, OrderType::Limit, 1001, 10, 2);
    EXPECT_EQ(r.error, Error::DuplicateId);
    EXPECT_EQ(r.events, 0u);
}

TEST_F(BookTest, OutOfBandPriceRejected) {
    // band is 1000 +/- 16 -> valid ticks [984, 1016)
    auto r = add(1, Side::Bid, OrderType::Limit, 2000, 10, 1);
    EXPECT_EQ(r.error, Error::OutOfBandPrice);
    EXPECT_EQ(book.resting_count(), 0u);
}

TEST_F(BookTest, InvalidArgsRejected) {
    auto r0 = book.add(OrderId{0}, Side::Bid, OrderType::Limit, Price{999},
                       Quantity{10}, Timestamp{1}, 0, ev, 64);
    EXPECT_EQ(r0.error, Error::InvalidArgs);
    auto r1 = book.add(OrderId{1}, Side::Bid, OrderType::Limit, Price{999},
                       Quantity{0}, Timestamp{1}, 0, ev, 64);
    EXPECT_EQ(r1.error, Error::InvalidArgs);
}

TEST_F(BookTest, EventBufferTooSmallIsAtomic) {
    add(1, Side::Ask, OrderType::Limit, 1001, 5, 1);
    // Would need 2 events (1 fill + Ack); buffer of 1 -> reject before mutating.
    auto r = book.add(OrderId{2}, Side::Bid, OrderType::Limit, Price{1005},
                      Quantity{5}, Timestamp{2}, 0, ev, 1);
    EXPECT_EQ(r.error, Error::EventBufferFull);
    EXPECT_EQ(r.events, 0u);
    EXPECT_EQ(book.resting_count(), 1u);  // untouched
    EXPECT_TRUE(book.debug_check_invariants());
}

TEST_F(BookTest, SelfTradeCancelResting) {
    OrderBookConfig c = test_config();
    c.self_trade = SelfTradePolicy::CancelResting;
    OrderBook b{c};
    Event e[16];

    b.add(OrderId{1}, Side::Bid, OrderType::Limit, Price{999}, Quantity{10},
          Timestamp{1}, 7, e, 16);
    // Ask from the same owner crosses its own bid: resting bid cancelled,
    // incoming ask rests (no asks crossed... bids are gone).
    auto r = b.add(OrderId{2}, Side::Ask, OrderType::Limit, Price{999},
                   Quantity{10}, Timestamp{2}, 7, e, 16);
    EXPECT_EQ(r.error, Error::Ok);
    ASSERT_EQ(r.events, 2u);
    EXPECT_EQ(view(e[0]).type, EventType::Cancel);
    EXPECT_EQ(view(e[0]).order_id, 1u);
    EXPECT_EQ(view(e[1]).type, EventType::Ack);
    EXPECT_EQ(b.resting_count(), 1u);
    EXPECT_TRUE(b.debug_check_invariants());
}

TEST_F(BookTest, SelfTradeCancelIncoming) {
    OrderBookConfig c = test_config();
    c.self_trade = SelfTradePolicy::CancelIncoming;
    OrderBook b{c};
    Event e[16];

    b.add(OrderId{1}, Side::Bid, OrderType::Limit, Price{999}, Quantity{10},
          Timestamp{1}, 7, e, 16);
    auto r = b.add(OrderId{2}, Side::Ask, OrderType::Limit, Price{999},
                   Quantity{10}, Timestamp{2}, 7, e, 16);
    EXPECT_EQ(r.error, Error::SelfTradeReject);
    ASSERT_EQ(r.events, 1u);
    EXPECT_EQ(view(e[0]).type, EventType::Reject);
    EXPECT_EQ(b.resting_count(), 1u);  // own bid untouched
    EXPECT_TRUE(b.debug_check_invariants());
}

TEST_F(BookTest, SelfTradeIgnoredForAnonymousOwners) {
    // owner == 0: no self-trade handling even under CancelResting.
    OrderBookConfig c = test_config();
    c.self_trade = SelfTradePolicy::CancelResting;
    OrderBook b{c};
    Event e[16];

    b.add(OrderId{1}, Side::Ask, OrderType::Limit, Price{1001}, Quantity{5},
          Timestamp{1}, 0, e, 16);
    auto r = b.add(OrderId{2}, Side::Bid, OrderType::Limit, Price{1005},
                   Quantity{5}, Timestamp{2}, 0, e, 16);
    EXPECT_EQ(r.error, Error::Ok);
    EXPECT_EQ(view(e[0]).type, EventType::Fill);  // normal fill, no cancel
    EXPECT_TRUE(b.debug_check_invariants());
}

TEST_F(BookTest, NoCrossedBookAfterSweep) {
    add(1, Side::Bid, OrderType::Limit, 999, 10, 1);
    add(2, Side::Bid, OrderType::Limit, 998, 10, 2);
    add(3, Side::Ask, OrderType::Limit, 1001, 10, 3);
    add(4, Side::Ask, OrderType::Limit, 1002, 10, 4);
    add(5, Side::Bid, OrderType::Limit, 1002, 25, 5);  // sweeps both asks, rests 5
    Price b{0}, a{0};
    EXPECT_TRUE(book.best_bid(b));
    EXPECT_EQ(b.ticks, 1002);
    EXPECT_FALSE(book.best_ask(a));  // asks fully swept
}

}  // namespace
}  // namespace kairos
