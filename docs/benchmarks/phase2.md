# Phase 2 Benchmarks

Measured 2026-10-03. Same caveats as Phase 1: shared, non-isolated 2-vCPU
environment, GCC 13.3, `-O3 -march=native`. Indicative only; re-run on
isolated pinned hardware with `perf stat` before publication.

## Replay throughput

File: 200,001 synthetic events (1 snapshot + 200,000 diffs, 400 levels),
14 MB. Command:

```
KAIROS_BENCH_CAPTURE=/tmp/bench_capture.kai \
  ./build/bench/bench/kairos_bench_replay
```

| Benchmark | Throughput |
|---|---|
| `BM_ReplayThroughput` | **~1.02M events/sec** |

This includes mmap page-in, L2 book apply (binary search + memmove on
~400 levels), and the FNV-1a determinism checksum per event. The null
consumer means strategy/fill-model cost is not included.

## What to measure next

- Replay throughput on a real multi-GB capture (page-cache effects,
  larger books).
- Fill-model overhead per event (queue attribution with Binomial
  sampling vs risk-averse).
- Latency-model sampling cost (empirical CDF binary search).
- End-to-end simulated backtest: events/sec with a resting order and
  account updates.
