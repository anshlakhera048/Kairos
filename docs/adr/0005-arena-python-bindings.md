# ADR 0005 — Arena Python bindings + LLM agent framework

Date: 2026-10-17
Status: accepted

## Context

The tournament arena (Phase 4) only ran C++ bots. Python researchers —
and especially LLM-agent builders — could not compete on the engine,
which blocked the entire "agent arena" direction: the differentiator is
agents trading on a real matching engine, not a toy simulator.

## Decision

1. **Expose the arena to Python** (`kairos_py.arena_run`) by reusing the
   existing `PyStrategy` adapter unchanged: its callbacks are templated on
   the Context type, and the arena's Context satisfies the same interface
   as the simulator's. The adapter moved to `python/py_strategy.hpp`,
   shared by `bindings.cpp` and the new `arena_bindings.cpp`. Result dicts
   use the exact `kairos_tournament` JSON keys so leaderboard scoring is
   shared.
2. **Fresh bot per seed.** `arena_run` takes a zero-arg `bot_factory`,
   matching the C++ tournament's fresh-state-per-seed discipline.
3. **Thinking budget, not thinking ban.** The arena's per-callback
   wall-clock budget applies to Python bots; the default in `arena_run` is
   5s (vs 5ms for C++) and the CLI exposes `--budget-s`. Rationale: the
   market runs in logical time, so a larger budget buys no market
   advantage — it only guards against hangs. Documented, not hidden.
4. **Agent framework** (`kairos/agents/`): `ArenaBot` base class (seven
   no-op callbacks), `snapshot()` helper, `LLMClient` (stdlib-only
   OpenAI-compatible chat — no new dependencies), `MockLLM` for offline
   runs, and `DebateTrader`, a reference analyst -> bull/bear debate ->
   trader -> rule-based-risk-gate agent. Fail-safe: any LLM error, bad
   JSON, or risk-gate breach means hold (never trade what it can't parse).
5. **PIC for the core archive.** Linking flow.cpp's vtables into the
   shared module required `-fPIC` on `kairos_core`; scoped to the Python
   build only (`POSITION_INDEPENDENT_CODE` in `python/CMakeLists.txt`) so
   engine codegen is untouched.

## Consequences

- `kairos arena --py-bot mybot.py:MyBot` runs Python bots on identical
  seeds/markets as the C++ bots; mixed tournaments print one leaderboard.
- Live LLM runs are non-deterministic by nature (model sampling); the
  engine underneath stays deterministic. `MockLLM` keeps demos/tests
  offline and reproducible.
- No new runtime dependencies: the LLM client is stdlib urllib.

## Alternatives considered

- **pybind11 trampoline + virtual Bot base** — rejected: more C++
  surface for the same result; the existing adapter already abstracts
  the boundary.
- **Adding `openai`/`anthropic` SDK dependencies** — rejected per the
  dependency rule; a 40-line urllib client covers every OpenAI-compatible
  endpoint (OpenAI, OpenRouter, Ollama).
