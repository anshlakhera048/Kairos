# LEARNING.md

Concepts behind the code, each in 2–3 lines — plain language, no hand-waving.
If I can't explain it, I don't understand it yet.

## Setup
- *(empty: scaffolding only)*

## Phase 1 — Order book + matching engine

- The differential test caught a real bug in the *reference*, not the engine:
  `std::prev(opp.rend().base())` is `std::prev(begin())` — undefined
  behavior. It happened to "work" just well enough to diverge only on ask
  takers. Lesson: the reference implementation needs the same care as the
  engine; differential testing only works if both sides are independently
  written *and* the harness actually exercises the paths (ask-taker sweeps
  did).
- Two engine bugs found by unit tests before the differential run: (1)
  `PostOnly` orders validated as non-crossing never rested (`rests` only
  allowed `Limit`); (2) `Ack.qty` carried the leftover remainder instead of
  the resting qty, so IOC/Market acks lied about what rested. Both were
  spec-clarity issues — the event contract ("Ack.qty = amount now resting")
  had to be precise before the code could be.
- `execute_add` originally returned void and swallowed the
  `SelfTradeReject` from the CancelIncoming path; the error code is part of
  the contract, so it now propagates.
- Separate counters for event seqs (dense 1..N) and order time-priority
  seqs. Sharing one counter "worked" but made the event stream harder to
  reason about and the reference harder to mirror.
- Backward-shift deletion in the id map: the cyclic "hole in [k, j]" test
  is the subtle part. The invariant checker + differential test are what
  make it trustworthy, not re-reading the code.
- Event-buffer atomicity via the `live_count + 1/2` bound check eliminated
  the need for a pre-scan pass or rollback logic. The FOK quantity check is
  the only read-only pre-scan, because all-or-nothing genuinely needs it.

## Phase 1 — Order Book + Matching Engine
- *(to be filled as the engine is built)*

## Phase 2 — Capture, Replay, Realism

- The egress proxy blocks websocket upgrades, so the recorder has two
  modes: `ws` (primary, for deployment) and `poll` (REST fallback, works
  everywhere). Same binary format, so the replay engine doesn't care.
  Lesson: design the capture path for the deployment environment, not the
  dev environment — and test the sync logic against a mock server.
- Coinbase's `level2` channel was chosen over Binance (geo-blocked from
  here) and Kraken: snapshot + sequenced diffs with a documented sync
  procedure, no auth for market data.
- Binary format v1: fixed-size everything (64B header, 48B event header,
  24B level entries), little-endian, CRC32 finalized at close. The C++
  reader walks it with pure pointer arithmetic. A Python writer + reader
  pair caught a struct-size miscount before any C++ was written.
- `-Wpedantic` rejects `__int128`. The account's notional intermediates
  moved to `src/account.cpp` with a file-scoped pragma, keeping the
  header clean. Both supported compilers (GCC, Clang) provide `__int128`.
- `p_ahead = 1.0` overflowed the binomial threshold (`1.0 * 2^64` isn't
  representable in uint64_t — UB that silently returned 0). Boundary
  special-cases (`<= 0.0`, `>= 1.0`) are exact and obvious.
- The probabilistic queue's "feed is authoritative" clamp (unattributed
  shrinkage must have been ahead) is optimistic — documented as a known
  failure mode, not hidden.
- Replay determinism is a checksum over event identity + resulting
  best bid/ask, not just event bytes: it catches book-reconstruction
  bugs, not just I/O corruption.

## Phase 3 — Strategy API, Market Maker, Research

- The simulator's timer rescheduling had no termination condition: after
  the replay ended, the timer kept the delivery queue non-empty forever
  (99% CPU hang). Fix: only reschedule while market data remains. Lesson:
  every recurring event needs an explicit stop rule.
- The strategy's account view must update at FILL REPORT time, not exchange
  time — otherwise the strategy can observe fills early via ctx.account().
  This was a real lookahead leak, caught by reasoning about delivery order,
  not by a test. The cheat tests verify the mechanism.
- A-S gamma must be O(1000) for tick-level inventory skew when sigma^2*T
  is O(1e-4). This is a calibration reality (gamma absorbs lot/tick units),
  not a model bug. Documented in the test.
- Synthetic market generator bug: diffs didn't remove stale levels when the
  mid moved, creating a crossed book that rejected 50% of post-only orders.
  The simulator was correct; the test data was wrong. Always verify the
  book isn't crossed in generated data.
- The realism gap is real and measurable: naive fill-at-touch overstates
  PnL by ~19% vs a realistic queue model (statistically significant via
  block bootstrap). Latency alone barely mattered for this maker strategy.
- Python bindings: pybind11's `PyContext` collides with Python.h's
  `PyContext` typedef — renamed to `KairosPyContext`. The Python A-S
  initially used symmetric spreads; the C++ uses inventory-asymmetric
  distances (the correct A-S). Fixed Python to match bit-exactly.
- Measured Python overhead honestly: 11.6x slower than C++ (2.7x binding
  + 4.3x strategy logic). Documented in docs/python-overhead.md — the
  spec asked for honesty, and the numbers justify the two-path design.

## Phase 4 — The Arena

- Live engine (not pre-generated flow) is the right call: the bot's
  orders rest in the book and get hit — market impact matters.
- Fill detection via `OrderBook::get_order()` polling (new read-only API).
  The engine's events are taker-centric; they don't identify makers.
- The seed orders must be tiny, or they absorb all flow via time priority
  and the bot never gets filled. Learned the hard way.
- Bots that requote on every book update never rest long enough to get
  hit. The FixedSpreadBot now only requotes when prices actually change.
- The cheater test verifies anti-cheat: 40 orders rejected by position
  limit, 0 fills. The limit works.
- OrderBook needed a configurable price band (was hardcoded to [-1024,
  1024]). Added `OrderBookConfig` to `Engine`.
