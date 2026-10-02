// Smoke benchmark: proves the Google Benchmark toolchain works end to end.
// It benchmarks something deliberately trivial (a Price comparison). Real
// microbenchmarks (add/cancel/match) arrive with the Phase 1 engine.

#include <benchmark/benchmark.h>

#include "kairos/types.hpp"

namespace {

void BM_PriceCompare(benchmark::State& state) {
    const kairos::Price a{100};
    const kairos::Price b{200};
    for (auto _ : state) {
        benchmark::DoNotOptimize(a < b);
    }
}
BENCHMARK(BM_PriceCompare);

}  // namespace

BENCHMARK_MAIN();
