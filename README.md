# Kairos

A from-scratch, low-latency **C++20** system with one deterministic core used in two modes:

1. **SIMULATOR** — replays recorded L2 market data into an order book, injects a strategy's orders with realistic order-entry latency and queue-position modelling, and produces fills you can trust.
2. **ARENA** — the same matching engine driven by simulated agents (noise traders, informed traders, other bots) so market-making bots compete on a leaderboard.

**Thesis:** most open-source backtesters assume fills at the price you see. Kairos models latency and queue position — and *validates* the model against real fills. Validation is the differentiator.

## Status

Early development. The build system, core types, developer tooling, and the
matching engine (Phase 1) are in place and fully tested. See the roadmap below.

## Roadmap

- **Phase 1 — Order book + matching engine (done).** Price-time priority
  matching engine: limit/market/IOC/FOK/post-only, modify semantics,
  self-trade policies, allocation-free event output. Differential-tested
  against a naive reference (~18M ops, adversarial distributions, ASan/UBSan
  clean). Benchmarks: ~100 ns/add, ~15 ns/fill at scale; see
  `docs/benchmarks/phase1.md`.
- **Phase 2 — Capture, replay, realism (next).** Market-data recorder, deterministic replay engine, pluggable latency and queue-position models, validation plan against real fills.
- **Phase 3 — Strategy API, market maker, research result (weeks 8–10).** Event-driven strategy API (C++ + Python bindings), Avellaneda–Stoikov reference market maker, and the "realism gap" experiment as a research note.
- **Phase 4 — The arena (weeks 11+).** Local-first tournament harness: deterministic flow generators, fair scoring, leaderboard. Hosted submissions are an explicit later stretch goal.

## Quickstart

```bash
# build + test (debug)
cmake --preset debug && cmake --build --preset debug && ctest --preset debug

# optimized build
cmake --preset release && cmake --build --preset release

# sanitizers (must be clean on every change)
./scripts/sanitizers.sh

# benchmarks (local, pinned core — never in CI)
./scripts/bench.sh
```

See `docs/benchmarks/METHODOLOGY.md` before trusting any number.

## Layout

```
include/kairos/   public headers (types, order book, engine, events)
src/              implementation
tests/            unit / differential / randomized tests
bench/            Google Benchmark + custom latency harness
tools/            recorder, replay CLI
python/           future pybind11 bindings
docs/             design notes, ADRs, benchmarks, LEARNING.md
scripts/          format, build, bench, sanitizer helpers
```

## Docs

- `AGENTS.md` — project brief: engineering rules and how we work. Read it first.
- `docs/adr/` — architecture decision records.
- `docs/benchmarks/` — methodology and results.
- `docs/LEARNING.md` — concepts behind the code, in plain language.

## License

MIT — see `LICENSE`.
