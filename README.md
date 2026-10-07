# Kairos

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](LICENSE)
![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)

A from-scratch, low-latency **C++20** system for market microstructure research,
built around one deterministic core used in two modes:

1. **Backtester** — replays recorded L2 market data into an order book, injects a
   strategy's orders with realistic entry latency and queue-position modelling,
   and produces fills you can actually trust.
2. **Arena** — the same matching engine driven by simulated agents (noise
   traders, informed traders, background makers, other bots), so market-making
   bots compete on a leaderboard under identical conditions.

**Why it exists:** most open-source backtesters assume you get filled at the
price you see. That assumption is wrong, and the error compounds: Kairos models
order-entry latency and queue position — and then *measures* the gap. On
synthetic data, naive fill-at-touch overstates market-maker PnL by ~19% versus
a realistic queue model ([research note](docs/research/realism-gap.md)).
Validation, not features, is the differentiator.

## Features

- **Deterministic matching engine** — price-time priority; limit, market, IOC,
  FOK, post-only; modify/cancel; self-trade policies. Allocation-free event
  output, fixed-capacity structures, no locks or exceptions in the hot path.
  Differential-tested against a naive reference (~18M adversarial ops,
  ASan/UBSan clean).
- **Market-data capture & replay** — Coinbase L2 recorder (websocket with REST
  fallback, no API key needed), fixed-size mmap-able binary format with CRC32,
  deterministic C++ replay at ~1M events/sec.
- **Realistic fill modelling** — constant, jittered, and empirical latency
  models; naive, risk-averse, and probabilistic queue-position models; simulated
  account with fees and PnL.
- **Event-driven Strategy API** — compile-time dispatch, no virtual calls; the
  same strategy code runs in the backtester and the arena. C++ and Python
  (pybind11) strategies with bit-identical A-S quotes.
- **Avellaneda–Stoikov market maker** — reference implementation (C++ + Python)
  with tested sigma/kappa estimators.
- **Tournament arena** — deterministic flow generators (noise, informed,
  background makers, momentum) and 5 example bots (A-S, fixed-spread, momentum,
  random, cheater) compete on the live engine in logical time. Identical
  latency, information, fees, and seeds per bot; PnL / inventory-penalty /
  risk-adjusted scoring; position, rate-limit, and callback time-budget
  anti-cheat; static HTML leaderboard.
- **Honest benchmarking** — methodology documented in
  [docs/benchmarks](docs/benchmarks/); benchmarks never run in CI (shared
  runners are too noisy).

## Quickstart

**Prerequisites:** CMake ≥ 3.24, a C++20 compiler (GCC or Clang), Python 3
(for the bindings and tooling), and internet access on first configure
(GoogleTest, Google Benchmark, and pybind11 come via FetchContent).

```bash
# All code lives on the dev branch (main is kept clean by design)
git clone -b dev https://github.com/anshlakhera048/Kairos.git
cd Kairos

# build (debug) + run all tests
./scripts/build.sh
ctest --preset debug

# optimized build
./scripts/build.sh release

# sanitizers — must be clean on every change
./scripts/sanitizers.sh
```

### Run a tournament

```bash
cmake --build --preset release --target kairos_tournament
./build/release/tools/kairos_tournament \
    --bots as,fixed,momentum,random,cheater \
    --seeds 50 --duration 20 --regime calm \
    --out /tmp/results.json

# leaderboard
python3 tools/arena/leaderboard.py --input /tmp/results.json --out /tmp/leaderboard.html
```

### Reproduce the realism-gap experiment

```bash
./scripts/realism_gap.sh ./docs/research/figures
```

This regenerates every figure and table in [docs/research/realism-gap.md](docs/research/realism-gap.md)
from a fixed-seed synthetic capture — the ~19% naive-PnL overstatement included.

### Python bindings

```bash
cmake --build --preset debug --target kairos_py
PYTHONPATH=python python3 -c "import kairos; print(kairos.__doc__)"
```

See [python/test_bindings.py](python/test_bindings.py) for the quote-parity check
and [docs/python-overhead.md](docs/python-overhead.md) for the honest numbers
(Python callbacks are ~11.6× slower than C++ — measure before you trade on it).

## Write your own bot

A bot is any class satisfying the Strategy concept — seven callbacks, no
inheritance required. It works unchanged in the backtester and the arena:

```cpp
#include "strategies/arena_bots.hpp"  // example bots to crib from

class MyBot {
public:
    template <typename Ctx>
    void on_book_update(Ctx& ctx) {
        // ctx.book()      -> delayed L2 view (no lookahead, ever)
        // ctx.account()   -> your positions and PnL
        // ctx.send_limit(side, price_ticks, qty_lots, post_only) -> order id
        // ctx.cancel(order_id)
        // ctx.now()       -> your logical timestamp (ns)
    }
    template <typename Ctx> void on_trade(Ctx&, const TradeInfo&) {}
    template <typename Ctx> void on_own_fill(Ctx& ctx, const SimFill&) {}
    template <typename Ctx> void on_ack(Ctx&, const AckInfo&) {}
    template <typename Ctx> void on_reject(Ctx&, std::uint64_t) {}
    template <typename Ctx> void on_cancel(Ctx&, std::uint64_t) {}
    template <typename Ctx> void on_timer(Ctx& ctx) {}
};
```

Two rules that will save you an afternoon:

- **Gate your requotes.** The arena delivers a book view on nearly every event-loop
  tick. Cancel/re-place only when your desired quotes actually change, or you
  will hammer the order-rate limiter (the A-S bot learned this the hard way —
  8.9M rejections before gating).
- **Mind your units.** The engine tracks positions in micro-lots (`qty_scale =
  1e6`); the A-S model takes inventory in whole lots. Convert, or one fill
  will throw your quotes 400k ticks off-touch.

Then register it in `tools/arena/run_tournament.cpp` and compete.

## Project layout

```
include/kairos/   public headers: types, order book, engine, events, models
src/              implementation
strategies/       example bots (arena) + A-S market maker
tests/            unit / differential / randomized tests
bench/            Google Benchmark + custom latency harness
tools/            recorder, replay, realism-gap experiment, tournament
python/           pybind11 bindings + Python strategies
docs/             design notes, ADRs, benchmarks, research, LEARNING.md
scripts/          build, format, bench, sanitizer, experiment helpers
```

## Documentation

- [docs/design/](docs/design/) — order book, matching, fill model, arena,
  data format, validation plan, hosting threat model.
- [docs/adr/](docs/adr/) — architecture decision records (why the core looks
  the way it does).
- [docs/benchmarks/](docs/benchmarks/) — methodology first, numbers second.
  Read `METHODOLOGY.md` before trusting any number.
- [docs/research/realism-gap.md](docs/research/realism-gap.md) — the experiment
  and what it found.
- [docs/LEARNING.md](docs/LEARNING.md) — concepts behind the code, in plain
  language. Start here if you're new to market microstructure.
- [AGENTS.md](AGENTS.md) — engineering rules for working in this codebase
  (human or agent). Read before contributing.

## Development status & limitations

Kairos is under active development on the `dev` branch (`main` is kept clean).
Phases 1–4 are implemented and tested:

| Phase | What | Status |
|---|---|---|
| 1 | Matching engine | Done — 84 tests, ASan/UBSan clean, differential-tested |
| 2 | Capture, replay, fill modelling | Done |
| 3 | Strategy API, A-S market maker, Python bindings | Done |
| 4 | Tournament arena + leaderboard | Done — 5 bots × 50 seeds reproducible |

Be aware of the following:

- **No live trading.** Nothing here connects to an exchange for order placement,
  and that is deliberate. The recorder is read-only market data.
- **Synthetic-first validation.** The realism-gap results are measured on
  synthetic captures; validation against real fills is planned
  ([docs/design/validation-plan.md](docs/design/validation-plan.md)), not done.
- **Single-threaded, single-machine.** The engine and arena are deterministic
  and single-threaded by design. There is no distributed deployment story.
- **Benchmarks are indicative.** Latency numbers were measured on specific
  hardware; see [docs/benchmarks/METHODOLOGY.md](docs/benchmarks/METHODOLOGY.md)
  and re-run on your own machine with `./scripts/bench.sh`.
- **Python is for research, not production quoting.** The bindings add ~11.6×
  overhead versus C++ strategies ([details](docs/python-overhead.md)).
- **CI is not yet live.** The workflow exists at `.github/workflows/ci.yml`;
  until it runs green, `./scripts/sanitizers.sh` + `ctest` are the bar.

## Contributing

Contributions are welcome — bug reports, calibration data, new example bots,
and documentation improvements especially. Please read
[CONTRIBUTING.md](CONTRIBUTING.md) and [AGENTS.md](AGENTS.md) first; the
engineering rules (no heap allocation in the hot path, strict warnings as
errors, sanitizers clean on every change) are non-negotiable and enforced in
review.

## License

MIT — see [LICENSE](LICENSE).
