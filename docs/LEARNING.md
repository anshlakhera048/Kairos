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
- *(to be filled)*

## Phase 3 — Strategy API, Market Maker, Research
- *(to be filled)*

## Phase 4 — The Arena
- *(to be filled)*
