// Randomized property tests for the OrderBook.
//
// These complement the differential test: instead of comparing against a
// reference, they assert properties that must hold for ANY valid execution:
// determinism (same seed => bit-identical event streams), no crossed book,
// fill accounting (fills never exceed order quantities), and structural
// invariants after every batch of ops.

#include <gtest/gtest.h>

#include <cstdint>
#include <random>
#include <unordered_map>
#include <vector>

#include "kairos/error.hpp"
#include "kairos/events.hpp"
#include "kairos/order_book.hpp"
#include "kairos/types.hpp"

namespace kairos {
namespace {

OrderBookConfig prop_config() {
    OrderBookConfig c;
    c.base_price = Price{50000};
    c.half_band = 256;
    c.max_orders = 1u << 16;
    return c;
}

struct Op {
    int kind;  // 0=add, 1=cancel, 2=modify
    std::uint64_t id;
    Side side = Side::Bid;
    OrderType type = OrderType::Limit;
    std::int64_t price = 0;
    std::int64_t qty = 0;
};

std::vector<Op> gen_ops(std::uint64_t seed, std::size_t n) {
    std::mt19937_64 rng(seed);
    std::vector<Op> ops;
    std::vector<std::uint64_t> live;
    std::uint64_t next_id = 1;
    std::uniform_int_distribution<int> pick(0, 99);
    for (std::size_t i = 0; i < n; ++i) {
        const int r = pick(rng);
        if (r < 75 || live.empty()) {
            Op op;
            op.kind = 0;
            op.id = next_id++;
            live.push_back(op.id);
            op.side = (rng() & 1) ? Side::Ask : Side::Bid;
            const int t = pick(rng);
            op.type = t < 60   ? OrderType::Limit
                      : t < 70 ? OrderType::Market
                      : t < 80 ? OrderType::Ioc
                      : t < 90 ? OrderType::Fok
                               : OrderType::PostOnly;
            std::uniform_int_distribution<std::int64_t> pd(-120, 120);
            op.price = 50000 + pd(rng);
            std::uniform_int_distribution<std::int64_t> qd(1, 40);
            op.qty = qd(rng);
            ops.push_back(op);
        } else if (r < 90) {
            std::uniform_int_distribution<std::size_t> d(0, live.size() - 1);
            ops.push_back(Op{1, live[d(rng)], Side::Bid, OrderType::Limit, 0, 0});
        } else {
            std::uniform_int_distribution<std::size_t> d(0, live.size() - 1);
            std::uniform_int_distribution<std::int64_t> pd(-120, 120);
            std::uniform_int_distribution<std::int64_t> qd(1, 40);
            ops.push_back(Op{2, live[d(rng)], Side::Bid, OrderType::Limit,
                             50000 + pd(rng), qd(rng)});
        }
    }
    return ops;
}

std::vector<Event> run_stream(const std::vector<Op>& ops) {
    OrderBook book(prop_config());
    std::vector<Event> out;
    std::vector<Event> buf(70000);
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        OrderBook::OpResult r{Error::Ok, 0};
        if (op.kind == 0) {
            r = book.add(OrderId{op.id}, op.side, op.type, Price{op.price},
                         Quantity{op.qty}, Timestamp{i}, 0, buf.data(),
                         buf.size());
        } else if (op.kind == 1) {
            r = book.cancel(OrderId{op.id}, Timestamp{i}, buf.data(),
                            buf.size());
        } else {
            r = book.modify(OrderId{op.id}, Price{op.price}, Quantity{op.qty},
                            Timestamp{i}, buf.data(), buf.size());
        }
        for (std::size_t k = 0; k < r.events; ++k) {
            out.push_back(buf[k]);
        }
        if ((i & 0x3FF) == 0) {
            EXPECT_TRUE(book.debug_check_invariants()) << "op " << i;
            if (!book.debug_check_invariants()) {
                break;
            }
        }
    }
    return out;
}

bool same_event(const Event& a, const Event& b) {
    return a.type == b.type && a.order_id.id == b.order_id.id &&
           a.ts.ns == b.ts.ns && a.seq == b.seq &&
           a.price.ticks == b.price.ticks && a.qty.lots == b.qty.lots;
}

TEST(Properties, DeterministicStreams) {
    // Same seed, two engine instances => bit-identical event streams.
    for (std::uint64_t seed : {1u, 7u, 42u}) {
        const auto ops = gen_ops(seed, 50000);
        const auto s1 = run_stream(ops);
        const auto s2 = run_stream(ops);
        ASSERT_EQ(s1.size(), s2.size()) << "seed " << seed;
        for (std::size_t i = 0; i < s1.size(); ++i) {
            EXPECT_TRUE(same_event(s1[i], s2[i]))
                << "seed " << seed << " event " << i;
            if (!same_event(s1[i], s2[i])) {
                break;
            }
        }
    }
}

TEST(Properties, FillAccounting) {
    // For every taker: sum(fill qty) <= order qty. Fills reference the
    // taker's id and the engine never over-fills.
    const auto ops = gen_ops(1234, 50000);
    OrderBook book(prop_config());
    std::vector<Event> buf(70000);
    std::unordered_map<std::uint64_t, std::int64_t> initial;
    std::unordered_map<std::uint64_t, std::int64_t> filled;
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        OrderBook::OpResult r{Error::Ok, 0};
        if (op.kind == 0) {
            initial[op.id] = op.qty;
            r = book.add(OrderId{op.id}, op.side, op.type, Price{op.price},
                         Quantity{op.qty}, Timestamp{i}, 0, buf.data(),
                         buf.size());
        } else if (op.kind == 1) {
            r = book.cancel(OrderId{op.id}, Timestamp{i}, buf.data(),
                            buf.size());
        } else {
            // Modify-replace re-uses the id with a new quantity: reset
            // accounting for the replaced order (Cancel+Ack sequence).
            r = book.modify(OrderId{op.id}, Price{op.price}, Quantity{op.qty},
                            Timestamp{i}, buf.data(), buf.size());
            if (r.error == Error::Ok && r.events > 0 &&
                buf[0].type == EventType::Cancel) {
                initial[op.id] = op.qty;
                filled[op.id] = 0;
            }
        }
        for (std::size_t k = 0; k < r.events; ++k) {
            if (buf[k].type == EventType::Fill) {
                filled[buf[k].order_id.id] += buf[k].qty.lots;
            }
        }
    }
    for (const auto& [id, f] : filled) {
        ASSERT_LE(f, initial[id]) << "over-fill on order " << id;
    }
}

TEST(Properties, NeverCrossed) {
    // Best bid < best ask after every batch of ops, on adversarial streams.
    const auto ops = gen_ops(999, 50000);
    OrderBook book(prop_config());
    std::vector<Event> buf(70000);
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        if (op.kind == 0) {
            book.add(OrderId{op.id}, op.side, op.type, Price{op.price},
                     Quantity{op.qty}, Timestamp{i}, 0, buf.data(),
                     buf.size());
        } else if (op.kind == 1) {
            book.cancel(OrderId{op.id}, Timestamp{i}, buf.data(), buf.size());
        } else {
            book.modify(OrderId{op.id}, Price{op.price}, Quantity{op.qty},
                        Timestamp{i}, buf.data(), buf.size());
        }
        if ((i & 0xFF) == 0) {
            Price b{0}, a{0};
            const bool hb = book.best_bid(b);
            const bool ha = book.best_ask(a);
            EXPECT_TRUE(!hb || !ha || b.ticks < a.ticks) << "op " << i;
        }
    }
}

}  // namespace
}  // namespace kairos
