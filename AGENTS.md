# AGENTS.md — Kairos engineering rules

Read this before writing code, human or agent. These rules are enforced in
review; they are not suggestions.

## The hot path (matching engine: `include/kairos/order_book.hpp`, `src/order_book.cpp`, `src/engine.cpp`)

- No heap allocation. No locks. No exceptions. No virtual calls.
- No `std::map` / `std::unordered_map`. No iostream.
- No floating-point prices or quantities — integer ticks and lots only.
- Time is explicit `uint64_t` nanoseconds. No wall-clock reads.

## Determinism

- Identical input + seed ⇒ identical output. Always.
- No wall-clock reads, no unseeded randomness in the deterministic core.
  (`std::chrono::steady_clock` appears only in anti-cheat time budgets, which
  are explicitly documented as non-deterministic.)

## Correctness process

- Unit, differential, property, and sanitizer testing. New engine behavior
  gets differential coverage against the naive reference where feasible.
- `./scripts/sanitizers.sh` must be clean on every change.
- GCC **and** Clang, strict warnings as errors — for our targets only, never
  leak flags into FetchContent dependencies.
- Bug fixes ship with a regression test that fails before and passes after.

## Performance honesty

- Never state an unmeasured number. Benchmarks run on pinned, isolated
  hardware via `./scripts/bench.sh` — never in CI, never on shared runners.
- Read `docs/benchmarks/METHODOLOGY.md` before quoting or adding a number.

## Dependencies

Ask before adding anything. Current set, via FetchContent only: GoogleTest,
Google Benchmark, pybind11. Nothing else.

## Docs & decisions

- Significant design decisions get an ADR in `docs/adr/`.
- Concepts worth teaching go in `docs/LEARNING.md`.
- Keep `README.md`'s status table and roadmap honest — update them with the
  change, not after.

## Boundaries

- The recorder is **read-only** market data. Nothing in this repo places real
  orders, and no change may add real-money order placement without explicit
  written confirmation from the maintainer, plus (at minimum): trade-only
  keys, hard order/position limits, a kill switch, and testnet first.
- Public-facing text (docs, commits, PRs) is professional: no prompt talk,
  no scaffolding jargon.

## Workflow

- All work lands on `dev` via PR; `main` stays clean.
- Keep PRs focused: one change, one reason. Describe *why*.
- Never commit `build/` trees, `*.so`/`*.o`, `__pycache__`, or local data
  captures.
