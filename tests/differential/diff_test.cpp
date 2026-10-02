// Differential test: feeds identical random operation streams to OrderBook
// and the naive reference implementation, and requires bit-identical event
// streams (type, order id, ts, seq, price, qty) and identical error codes.
//
// Usage: diff_test [--ops N] [--seeds S] [--mode balanced|heavy-cancels|deep-sweeps|tight]
// Adversarial distributions stress cancels, sweeps, and tight spreads.

#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "kairos/error.hpp"
#include "kairos/events.hpp"
#include "kairos/order_book.hpp"
#include "kairos/types.hpp"
#include "reference.hpp"

namespace {

enum class OpKind { Add, Cancel, Modify };

struct Op {
    OpKind kind = OpKind::Add;
    std::uint64_t id = 0;
    kairos::Side side = kairos::Side::Bid;
    kairos::OrderType type = kairos::OrderType::Limit;
    std::int64_t price = 0;
    std::int64_t qty = 0;
    std::uint32_t owner = 0;
};

const char* to_str(kairos::OrderType t) {
    switch (t) {
        case kairos::OrderType::Limit: return "Limit";
        case kairos::OrderType::Market: return "Market";
        case kairos::OrderType::Ioc: return "Ioc";
        case kairos::OrderType::Fok: return "Fok";
        case kairos::OrderType::PostOnly: return "PostOnly";
    }
    return "?";
}

std::string describe(const Op& op) {
    char buf[256];
    if (op.kind == OpKind::Add) {
        std::snprintf(buf, sizeof(buf), "Add id=%llu %s %s price=%lld qty=%lld owner=%u",
                      (unsigned long long)op.id,
                      op.side == kairos::Side::Bid ? "Bid" : "Ask",
                      to_str(op.type), (long long)op.price, (long long)op.qty,
                      op.owner);
    } else if (op.kind == OpKind::Cancel) {
        std::snprintf(buf, sizeof(buf), "Cancel id=%llu",
                      (unsigned long long)op.id);
    } else {
        std::snprintf(buf, sizeof(buf), "Modify id=%llu price=%lld qty=%lld",
                      (unsigned long long)op.id, (long long)op.price,
                      (long long)op.qty);
    }
    return buf;
}

struct Gen {
    std::mt19937_64 rng;
    std::vector<std::uint64_t> live_ids;
    std::uint64_t next_id = 1;
    int mode = 0;  // 0 balanced, 1 heavy-cancels, 2 deep-sweeps, 3 tight
    static constexpr std::int64_t kBase = 100000;

    std::uint64_t pick_live() {
        std::uniform_int_distribution<std::size_t> d(0, live_ids.size() - 1);
        return live_ids[d(rng)];
    }

    Op next() {
        std::uniform_int_distribution<int> pick(0, 99);
        const int r = pick(rng);
        Op op;

        int add_w = 80, cancel_w = 12;
        if (mode == 1) {  // heavy cancels
            add_w = 50;
            cancel_w = 40;
        }

        if (r < add_w) {
            op.kind = OpKind::Add;
            op.id = next_id++;
            live_ids.push_back(op.id);
            std::uniform_int_distribution<int> side_d(0, 1);
            op.side = side_d(rng) ? kairos::Side::Ask : kairos::Side::Bid;
            int t = pick(rng);
            if (mode == 2 && t < 30) {
                op.type = kairos::OrderType::Market;  // deep sweeps
            } else if (t < 55) {
                op.type = kairos::OrderType::Limit;
            } else if (t < 65) {
                op.type = kairos::OrderType::Market;
            } else if (t < 75) {
                op.type = kairos::OrderType::Ioc;
            } else if (t < 85) {
                op.type = kairos::OrderType::Fok;
            } else {
                op.type = kairos::OrderType::PostOnly;
            }
            // Prices: tight around the base (dense, adversarial book).
            const std::int64_t spread = (mode == 3) ? 3 : 20;
            std::uniform_int_distribution<std::int64_t> pd(-spread, spread);
            op.price = kBase + pd(rng);
            if (pick(rng) < 3) {  // occasionally far: band edges
                std::uniform_int_distribution<std::int64_t> fd(-900, 900);
                op.price = kBase + fd(rng);
            }
            std::uniform_int_distribution<std::int64_t> qd(1, 50);
            op.qty = qd(rng);
            if (pick(rng) < 10) {  // 10% carry a nonzero owner (self-trade paths)
                std::uniform_int_distribution<std::uint32_t> od(1, 3);
                op.owner = od(rng);
            }
        } else if (r < add_w + cancel_w && !live_ids.empty()) {
            op.kind = OpKind::Cancel;
            if (pick(rng) < 90) {
                op.id = pick_live();  // 90% live id
            } else {
                std::uniform_int_distribution<std::uint64_t> idd(1, next_id + 100);
                op.id = idd(rng);  // 10% random: UnknownId path
            }
        } else if (!live_ids.empty()) {
            op.kind = OpKind::Modify;
            op.id = pick_live();
            std::uniform_int_distribution<std::int64_t> pd(-25, 25);
            op.price = kBase + pd(rng);
            std::uniform_int_distribution<std::int64_t> qd(1, 60);
            op.qty = qd(rng);
        } else {
            return next();  // no live ids: fall back to an add
        }
        return op;
    }
};

bool events_equal(const kairos::Event& a, const kairos::Event& b) {
    return a.type == b.type && a.order_id.id == b.order_id.id &&
           a.ts.ns == b.ts.ns && a.seq == b.seq &&
           a.price.ticks == b.price.ticks && a.qty.lots == b.qty.lots;
}

void print_event(const kairos::Event& e) {
    std::printf("    type=%d id=%llu ts=%llu seq=%llu price=%lld qty=%lld\n",
                (int)e.type, (unsigned long long)e.order_id.id,
                (unsigned long long)e.ts.ns, (unsigned long long)e.seq,
                (long long)e.price.ticks, (long long)e.qty.lots);
}

kairos::OrderBook::OpResult apply_engine(kairos::OrderBook& b, const Op& op,
                                         std::uint64_t ts, kairos::Event* out,
                                         std::size_t cap) {
    if (op.kind == OpKind::Add) {
        return b.add(kairos::OrderId{op.id}, op.side, op.type,
                     kairos::Price{op.price}, kairos::Quantity{op.qty},
                     kairos::Timestamp{ts}, op.owner, out, cap);
    }
    if (op.kind == OpKind::Cancel) {
        return b.cancel(kairos::OrderId{op.id}, kairos::Timestamp{ts}, out, cap);
    }
    return b.modify(kairos::OrderId{op.id}, kairos::Price{op.price},
                    kairos::Quantity{op.qty}, kairos::Timestamp{ts}, out, cap);
}

kairos::reference::RefBook::OpResult apply_ref(kairos::reference::RefBook& b,
                                               const Op& op, std::uint64_t ts,
                                               kairos::Event* out,
                                               std::size_t cap) {
    if (op.kind == OpKind::Add) {
        return b.add(kairos::OrderId{op.id}, op.side, op.type,
                     kairos::Price{op.price}, kairos::Quantity{op.qty},
                     kairos::Timestamp{ts}, op.owner, out, cap);
    }
    if (op.kind == OpKind::Cancel) {
        return b.cancel(kairos::OrderId{op.id}, kairos::Timestamp{ts}, out, cap);
    }
    return b.modify(kairos::OrderId{op.id}, kairos::Price{op.price},
                    kairos::Quantity{op.qty}, kairos::Timestamp{ts}, out, cap);
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t n_ops = 200000;
    int n_seeds = 3;
    int mode = 0;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--ops" && i + 1 < argc) {
            n_ops = std::stoull(argv[++i]);
        } else if (a == "--seeds" && i + 1 < argc) {
            n_seeds = std::stoi(argv[++i]);
        } else if (a == "--mode" && i + 1 < argc) {
            std::string m = argv[++i];
            mode = (m == "heavy-cancels") ? 1
                   : (m == "deep-sweeps") ? 2
                   : (m == "tight")       ? 3
                                          : 0;
        }
    }

    // Exercise self-trade policies on alternating seeds.
    const kairos::SelfTradePolicy policies[3] = {
        kairos::SelfTradePolicy::Allow, kairos::SelfTradePolicy::CancelResting,
        kairos::SelfTradePolicy::CancelIncoming};

    std::vector<kairos::Event> ebuf(70000), rbuf(70000);
    std::uint64_t total_ops = 0;

    for (int s = 0; s < n_seeds; ++s) {
        kairos::OrderBookConfig ecfg;
        ecfg.base_price = kairos::Price{Gen::kBase};
        ecfg.half_band = 1024;
        ecfg.max_orders = 1u << 16;
        ecfg.self_trade = policies[s % 3];
        kairos::OrderBook engine(ecfg);

        kairos::reference::RefConfig rcfg;
        rcfg.base_price = kairos::Price{Gen::kBase};
        rcfg.half_band = 1024;
        rcfg.max_orders = 1u << 16;
        rcfg.self_trade = policies[s % 3];
        kairos::reference::RefBook ref(rcfg);

        Gen gen;
        gen.rng = std::mt19937_64(0xC10C + static_cast<std::uint64_t>(s));
        gen.mode = mode;
        std::vector<Op> log;
        log.reserve(static_cast<std::size_t>(n_ops));

        for (std::uint64_t i = 0; i < n_ops; ++i) {
            const Op op = gen.next();
            log.push_back(op);
            const auto er =
                apply_engine(engine, op, i, ebuf.data(), ebuf.size());
            const auto rr = apply_ref(ref, op, i, rbuf.data(), rbuf.size());

            bool same = (er.error == rr.error) && (er.events == rr.events);
            for (std::size_t k = 0; same && k < er.events; ++k) {
                same = events_equal(ebuf[k], rbuf[k]);
            }
            if (!same) {
                std::printf("DIVERGENCE seed=%d op=%llu\n", s,
                            (unsigned long long)i);
                std::printf("  op: %s\n", describe(op).c_str());
                std::printf("  engine: error=%s events=%zu\n",
                            kairos::to_string(er.error), er.events);
                for (std::size_t k = 0; k < er.events && k < 8; ++k) {
                    print_event(ebuf[k]);
                }
                std::printf("  ref:    error=%s events=%zu\n",
                            kairos::to_string(rr.error), rr.events);
                for (std::size_t k = 0; k < rr.events && k < 8; ++k) {
                    print_event(rbuf[k]);
                }
                const std::size_t from =
                    log.size() > 20 ? log.size() - 20 : 0;
                std::printf("  last %zu ops:\n", log.size() - from);
                for (std::size_t k = from; k < log.size(); ++k) {
                    std::printf("    [%zu] %s\n", k, describe(log[k]).c_str());
                }
                char path[128];
                std::snprintf(path, sizeof(path), "/tmp/kairos_diverge_s%d.txt", s);
                if (FILE* f = std::fopen(path, "w")) {
                    for (std::size_t k = 0; k < log.size(); ++k) {
                        std::fprintf(f, "[%zu] %s\n", k,
                                     describe(log[k]).c_str());
                    }
                    std::fclose(f);
                    std::printf("  full op log: %s\n", path);
                }
                return 1;
            }
            if ((i & 0xFFFF) == 0 && !engine.debug_check_invariants()) {
                std::printf("INVARIANT FAILURE seed=%d op=%llu\n", s,
                            (unsigned long long)i);
                return 1;
            }
        }
        total_ops += n_ops;
        std::printf("seed %d: %llu ops, no divergence\n", s,
                    (unsigned long long)n_ops);
    }
    std::printf("OK: %llu ops x %d seeds, engine == reference\n",
                (unsigned long long)n_ops, n_seeds);
    return 0;
}
