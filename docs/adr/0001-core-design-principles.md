# ADR-0001: Core Design Principles

Date: 2026-10-02
Status: Accepted

Captures the non-negotiable engineering rules from `AGENTS.md` (project brief).
These constrain every design decision in every phase. Changing them requires a
new ADR.

## 1. Hot-path rules

The hot path (matching, book updates, event loop, strategy callbacks) must have:

- **No heap allocation.** All memory pre-allocated; pools and free lists.
- **No locks.** Single-threaded core; concurrency only at the edges via queues.
- **No exceptions.** Errors are return codes / `std::expected`-style outcomes.
- **No virtual calls.** Compile-time dispatch (templates, concepts, CRTP).
- **No `std::map` / `std::unordered_map`.** Cache-hostile node-based containers
  are banned on the hot path; use flat arrays, intrusive lists, open addressing.
- **No iostream.** No formatted I/O anywhere near measurement.
- **No floating point for prices or quantities.** Fixed-point integers only
  (price in ticks, quantity in lots) — exact, deterministic, single-cycle.

## 2. Determinism

Same input + same seed ⇒ bit-identical output. No wall-clock reads and no
unseeded randomness inside the core. Time is an explicit input (uint64
nanoseconds).

## 3. Single-threaded core

One deterministic core. Concurrency exists only at the edges (recorder, I/O),
communicating through clearly defined queues.

## 4. Data-oriented design

Think in cache lines, memory layout and branch predictability, not in class
hierarchies. Measure layout effects; don't assume them.

## 5. Verification standard

- Unit tests, a differential test against a slow naive reference
  implementation, and randomized/property tests.
- ASan + UBSan clean on every change (TSan where threads exist).
- clang and gcc both build clean with `-Wall -Wextra -Wpedantic -Werror`.

## 6. Benchmark honesty

- Google Benchmark plus a custom HDR-style latency histogram.
- Pinned core, warmed up; report p50/p99/p99.9/max, never only averages.
- Avoid coordinated omission. Document methodology (`docs/benchmarks/`).
- **Never state a performance number that was not measured.** If it can't be
  run here, say so and give the exact command to run.

## 7. Working agreements

- Plan first for non-trivial tasks; wait for OK before large code writes.
- Small, reviewable steps; each step compiles and has tests.
- Record important decisions as ADRs; keep `docs/LEARNING.md` current.
- Definition of done: builds clean (clang+gcc), tests pass, sanitizers clean,
  benchmarks run, docs updated, no silent TODOs.
