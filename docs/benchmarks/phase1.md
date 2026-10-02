# Phase 1 Benchmarks — Order Book

Date: 2026-10-03. Engine: `OrderBook` as of Phase 1 (`dev` branch).
Harness: Google Benchmark + HDR-style `LatencyHistogram`
(`bench/latency_harness.hpp`).

## Results

Microbenchmarks (median of 3 repetitions, fixed iteration counts):

| Benchmark | Time/op | Throughput | Notes |
|---|---|---|---|
| Add resting limit | ~100 ns | ~10.0M ops/s | non-crossing bid, rests |
| Cancel | ~71 ns | ~14.0M ops/s | hash lookup + unlink + map erase |
| Modify (reduce, in-place) | ~114 ns | ~8.8M ops/s | keeps queue priority |
| Modify (replace) | ~117 ns | ~8.5M ops/s | cancel/replace, loses priority |
| Sweep 10 fills, 1 level | 660 ns | 15.2M fills/s | ~66 ns/fill |
| Sweep 100 fills, 1 level | 1.76 µs | 57.1M fills/s | ~17.6 ns/fill |
| Sweep 1000 fills, 1 level | 15.4 µs | 63.5M fills/s | ~15.4 ns/fill |
| Sweep 20 fills, 2 levels | 740 ns | 27.0M fills/s | |
| Sweep 100 fills, 10 levels | 1.85 µs | 54.0M fills/s | |
| Sweep 500 fills, 50 levels | 7.55 µs | 66.3M fills/s | ~15 ns/fill |
| Mixed flow (55% add / 20% cancel / 10% modify / 15% aggressive) | ~151 ns | ~6.7M ops/s | pre-loaded 80k-order book |

Per-fill cost falls with sweep size (66 → 15 ns): the fixed per-operation
overhead (validation, best-level lookup, Ack emission) amortizes over more
fills. This is the expected shape, not a measurement artifact.

Latency distribution for a resting add (`BM_AddLatencyDist`, 100k samples,
HDR-style histogram; values are conservative bucket upper bounds):

| Percentile | Latency |
|---|---|
| p50 | ~191 ns |
| p99 | ~354 ns |
| p99.9 | ~731 ns |
| max | 12–339 µs (outliers; see methodology) |

The distribution run includes `std::chrono` timing overhead per sample
(~20–40 ns), so p50 reads slightly higher than the microbenchmark's 100 ns.
The max outliers are scheduling noise from the shared VM (see below), not
engine behavior — the histogram's job is to make them visible rather than
hidden in an average.

## Methodology (and why these numbers are indicative, not publishable)

Per `docs/benchmarks/METHODOLOGY.md`:

- **Machine:** shared, non-isolated VM (2 vCPUs, 1.5 GHz reported), no CPU
  pinning, no frequency scaling control, noisy neighbors possible. The
  `max` outliers above are almost certainly preemptions.
- **No coordinated-omission avoidance** beyond the harness recording each
  sample independently; the mixed flow does not simulate arrival processes.
- **No `perf stat` data** (cache/branch misses): `perf` is not usable in this
  container. Run `./scripts/bench.sh` on a pinned, isolated core and add
  `perf stat -e cache-misses,branch-misses` for the hardware-counter story.
- **Compiler:** GCC 13.3, `-O3`, default config (1M-order pool, ±1024-tick
  band). Clang numbers not yet measured.
- **Reproduce:** `cmake --preset bench && cmake --build --preset bench &&
  ./build/bench/bench/kairos_bench --benchmark_repetitions=3`.
  `scripts/bench.sh` wraps this; set `KAIROS_BENCH_CPU` to pin cores.

## What was fixed while benchmarking

Two harness bugs, both caught because the numbers looked wrong:

1. Google Benchmark auto-scaling (`--benchmark_min_time`) scaled iterations
   past the pool-exhaustion `break`, which capped actual work below
   `min_time` — the framework then scaled forever (10+ minute hang). Fix:
   fixed `->Iterations()` per benchmark, sized to ≤50% of pool; per-arg
   iteration counts via `BENCHMARK_TEMPLATE` (chained `->Arg()->Iterations()`
   silently applies the last value to all args).
2. `break` inside `for (auto _ : state)` makes the framework report
   **0 iterations** (and a regex removing the breaks also ate the `++id`
   increment, so Cancel measured the `UnknownId` fast path at 4.5 ns).
   Fix: no breaks in benchmark loops; loop ids advance unconditionally.

## TODO

- [ ] Re-run on isolated, pinned hardware with `perf stat` counters.
- [ ] Clang `-O3` numbers for the compiler comparison.
- [ ] TSan build of the benchmarks once threading exists at the edges
      (engine is single-threaded; nothing to check yet).
