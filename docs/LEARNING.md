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
- *(to be filled)*

## Phase 4 — The Arena
- *(to be filled)*
