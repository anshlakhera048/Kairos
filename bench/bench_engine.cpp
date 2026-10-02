// Phase 1 engine benchmarks: microbenchmarks for each hot-path operation
// plus a realistic mixed order flow. Latency distributions (p50/p99/p99.9/max)
// come from the HDR-style harness; Google Benchmark reports throughput.
//
// Methodology notes: see docs/benchmarks/METHODOLOGY.md and
// docs/benchmarks/phase1.md. Numbers from a shared, non-isolated VM are
// indicative only.

#include <benchmark/benchmark.h>

#include <chrono>
#include <cstdint>
#include <vector>

#include "kairos/order_book.hpp"
#include "latency_harness.hpp"

namespace {

constexpr std::int64_t kBase = 100000;

kairos::OrderBookConfig bench_config() {
    kairos::OrderBookConfig c;
    c.base_price = kairos::Price{kBase};
    c.half_band = 1024;
    c.max_orders = 1u << 20;
    return c;
}

// Shared large event buffer: every op needs at most live_count+2 events.
struct BenchBook {
    kairos::OrderBook book{bench_config()};
    std::vector<kairos::Event> ev{(1u << 20) + 2};
    std::uint64_t next_id = 1;

    kairos::OrderBook::OpResult add(kairos::Side side, kairos::OrderType type,
                                    std::int64_t price, std::int64_t qty,
                                    std::uint32_t owner = 0) {
        const std::uint64_t id = next_id++;
        return book.add(kairos::OrderId{id}, side, type, kairos::Price{price},
                        kairos::Quantity{qty}, kairos::Timestamp{id}, owner,
                        ev.data(), ev.size());
    }
};

// Add a non-crossing limit order that rests in the book.
void BM_AddResting(benchmark::State& state) {
    BenchBook b;
    for (auto _ : state) {
        // Bids below the base never cross (no asks in this book).
        const std::int64_t px = kBase - 10 - (b.next_id % 500);
        auto r = b.add(kairos::Side::Bid, kairos::OrderType::Limit, px, 10);
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_AddResting)->Iterations(400000);

// Cancel a resting order.
void BM_Cancel(benchmark::State& state) {
    BenchBook b;
    const std::uint64_t n = 200000;
    for (std::uint64_t i = 0; i < n; ++i) {
        b.add(kairos::Side::Bid, kairos::OrderType::Limit,
              kBase - 10 - (i % 500), 10);
    }
    std::uint64_t id = 1;
    for (auto _ : state) {
        auto r = b.book.cancel(kairos::OrderId{id}, kairos::Timestamp{id},
                               b.ev.data(), b.ev.size());
        benchmark::DoNotOptimize(r);
        ++id;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_Cancel)->Iterations(200000);

// Sweep K resting 1-lot orders at a single ask level with one market order.
// Setup (resting adds) is pause-timed; only the sweep is measured.
template <std::uint64_t K>
void BM_MatchSweepSingleLevel(benchmark::State& state) {
    BenchBook b;
    for (auto _ : state) {
        state.PauseTiming();
        for (std::uint64_t i = 0; i < K; ++i) {
            b.add(kairos::Side::Ask, kairos::OrderType::Limit, kBase + 1, 1);
        }
        state.ResumeTiming();
        const std::uint64_t id = b.next_id++;
        auto r = b.book.add(kairos::OrderId{id}, kairos::Side::Bid,
                            kairos::OrderType::Market, kairos::Price{0},
                            kairos::Quantity{(std::int64_t)K},
                            kairos::Timestamp{id}, 0, b.ev.data(),
                            b.ev.size());
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations() * K);  // fills
}
BENCHMARK_TEMPLATE(BM_MatchSweepSingleLevel, 10)->Iterations(20000);
BENCHMARK_TEMPLATE(BM_MatchSweepSingleLevel, 100)->Iterations(3000);
BENCHMARK_TEMPLATE(BM_MatchSweepSingleLevel, 1000)->Iterations(300);

// Sweep across L levels (10 orders per level) with one crossing limit order.
template <std::uint64_t L>
void BM_MatchSweepMultiLevel(benchmark::State& state) {
    BenchBook b;
    for (auto _ : state) {
        state.PauseTiming();
        for (std::uint64_t lv = 0; lv < L; ++lv) {
            for (int i = 0; i < 10; ++i) {
                b.add(kairos::Side::Ask, kairos::OrderType::Limit,
                      kBase + 1 + (std::int64_t)lv, 1);
            }
        }
        state.ResumeTiming();
        const std::uint64_t id = b.next_id++;
        auto r = b.book.add(kairos::OrderId{id}, kairos::Side::Bid,
                            kairos::OrderType::Limit,
                            kairos::Price{kBase + (std::int64_t)L},
                            kairos::Quantity{(std::int64_t)(L * 10)},
                            kairos::Timestamp{id}, 0, b.ev.data(),
                            b.ev.size());
        benchmark::DoNotOptimize(r);
    }
    state.SetItemsProcessed(state.iterations() * L * 10);
}
BENCHMARK_TEMPLATE(BM_MatchSweepMultiLevel, 2)->Iterations(15000);
BENCHMARK_TEMPLATE(BM_MatchSweepMultiLevel, 10)->Iterations(3000);
BENCHMARK_TEMPLATE(BM_MatchSweepMultiLevel, 50)->Iterations(600);

// In-place quantity reduction (keeps queue priority).
void BM_ModifyReduce(benchmark::State& state) {
    BenchBook b;
    const std::uint64_t n = 200000;
    for (std::uint64_t i = 0; i < n; ++i) {
        b.add(kairos::Side::Bid, kairos::OrderType::Limit,
              kBase - 10 - (i % 500), 100);
    }
    std::uint64_t id = 1;
    for (auto _ : state) {
        auto r = b.book.modify(kairos::OrderId{id}, kairos::Price{kBase - 10 - static_cast<std::int64_t>(id % 500)},
                               kairos::Quantity{50}, kairos::Timestamp{id},
                               b.ev.data(), b.ev.size());
        benchmark::DoNotOptimize(r);
        ++id;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ModifyReduce)->Iterations(200000);

// Cancel/replace modify (price change: loses priority).
void BM_ModifyReplace(benchmark::State& state) {
    BenchBook b;
    const std::uint64_t n = 200000;
    for (std::uint64_t i = 0; i < n; ++i) {
        b.add(kairos::Side::Bid, kairos::OrderType::Limit,
              kBase - 10 - (i % 500), 100);
    }
    std::uint64_t id = 1;
    for (auto _ : state) {
        auto r = b.book.modify(kairos::OrderId{id}, kairos::Price{kBase - 20 - static_cast<std::int64_t>(id % 500)},
                               kairos::Quantity{100}, kairos::Timestamp{id},
                               b.ev.data(), b.ev.size());
        benchmark::DoNotOptimize(r);
        ++id;
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_ModifyReplace)->Iterations(200000);

// Realistic mixed flow: seeded stream of adds (mostly limit near touch),
// cancels, modifies, and aggressive orders against a pre-loaded book.
void BM_MixedFlow(benchmark::State& state) {
    BenchBook b;
    // Pre-load a dense book: 40k bids, 40k asks near the touch.
    for (std::uint64_t i = 0; i < 40000; ++i) {
        b.add(kairos::Side::Bid, kairos::OrderType::Limit, kBase - 1 - (i % 20),
              1 + (i % 10));
        b.add(kairos::Side::Ask, kairos::OrderType::Limit, kBase + 1 + (i % 20),
              1 + (i % 10));
    }
    std::uint64_t rng = 0x12345678u;
    auto next_rand = [&]() {
        rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
        return rng >> 33;
    };
    for (auto _ : state) {
        const std::uint64_t r = next_rand() % 100;
        kairos::OrderBook::OpResult res{kairos::Error::Ok, 0};
        if (r < 55) {
            const bool bid = (next_rand() & 1) != 0;
            const std::int64_t px =
                kBase + (bid ? -1 : 1) * (1 + (std::int64_t)(next_rand() % 15));
            res = b.add(bid ? kairos::Side::Bid : kairos::Side::Ask,
                        kairos::OrderType::Limit, px, 1 + (next_rand() % 20));
        } else if (r < 75) {
            // cancel a pseudo-random live-ish id
            const std::uint64_t id = 1 + next_rand() % (b.next_id - 1);
            res = b.book.cancel(kairos::OrderId{id}, kairos::Timestamp{id},
                                b.ev.data(), b.ev.size());
        } else if (r < 85) {
            const std::uint64_t id = 1 + next_rand() % (b.next_id - 1);
            res = b.book.modify(kairos::OrderId{id},
                                kairos::Price{kBase - 1 - static_cast<std::int64_t>(next_rand() % 15)},
                                kairos::Quantity{1 + static_cast<std::int64_t>(next_rand() % 20)},
                                kairos::Timestamp{id}, b.ev.data(),
                                b.ev.size());
        } else {
            // aggressive: market or crossing limit
            const bool bid = (next_rand() & 1) != 0;
            res = b.add(bid ? kairos::Side::Bid : kairos::Side::Ask,
                        kairos::OrderType::Market, 0, 1 + (next_rand() % 30));
        }
        benchmark::DoNotOptimize(res);
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_MixedFlow)->Iterations(200000);

// Per-operation latency distribution for a resting add, via the HDR-style
// harness. Reports p50/p99/p99.9/max as counters (conservative upper bounds).
void BM_AddLatencyDist(benchmark::State& state) {
    using clock = std::chrono::high_resolution_clock;
    BenchBook b;
    kairos::bench::LatencyHistogram hist;
    const int kSamples = 100000;
    for (auto _ : state) {
        hist.reset();
        for (int i = 0; i < kSamples; ++i) {
            const std::uint64_t id = b.next_id;
            const std::int64_t px = kBase - 10 - (id % 500);
            const auto t0 = clock::now();
            auto r = b.add(kairos::Side::Bid, kairos::OrderType::Limit, px, 10);
            const auto t1 = clock::now();
            benchmark::DoNotOptimize(r);
            hist.record(static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0)
                    .count()));
        }
        state.counters["p50_ns"] = static_cast<double>(hist.percentile(50));
        state.counters["p99_ns"] = static_cast<double>(hist.percentile(99));
        state.counters["p99.9_ns"] =
            static_cast<double>(hist.percentile(99.9));
        state.counters["max_ns"] = static_cast<double>(hist.max());
        state.SetItemsProcessed(hist.count());
    }
}
BENCHMARK(BM_AddLatencyDist)->Iterations(3);

}  // namespace

BENCHMARK_MAIN();
