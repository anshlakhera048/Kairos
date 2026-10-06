# Contributing to Kairos

Thanks for your interest. Kairos is a small, carefully-engineered codebase —
contributions are welcome, and the bar is "would this survive review by someone
who reads matching-engine code for fun."

## Ways to contribute

- **Bug reports** — with a reproducer. A failing test is worth a thousand
  words; a seed + config that reproduces a tournament anomaly is nearly as good.
- **Example bots** — new strategies for the arena (`strategies/`). Interesting
  beats profitable; well-commented beats clever.
- **Calibration & validation** — real fill data, better queue models, latency
  measurements. This is the project's scarcest resource.
- **Documentation** — especially `docs/LEARNING.md` and anything that helps a
  newcomer run their first tournament in under 15 minutes.
- **Performance** — measured, reproducible improvements with methodology noted
  (see `docs/benchmarks/METHODOLOGY.md`). Never state an unmeasured number.

## Getting started

```bash
git clone https://github.com/anshlakhera048/Kairos.git
cd Kairos
./scripts/build.sh            # debug build
ctest --preset debug          # full test suite
./scripts/sanitizers.sh       # ASan+UBSan — must be clean
```

Work on a branch off `dev`, not `main`. Open a PR against `dev`.

## Engineering rules

Read [AGENTS.md](AGENTS.md) before writing code — it is short and
non-negotiable. The headline rules:

- **Hot path** (matching engine): no heap allocation, no locks, no exceptions,
  no virtual calls, no `std::map`/`std::unordered_map`, no iostream, no
  floating-point prices. Integer ticks, `uint64_t` nanoseconds.
- **Determinism**: identical input + seed ⇒ identical output. No wall-clock
  reads or unseeded randomness in the deterministic core.
- **Strict warnings as errors** for our targets (never leak flags into
  FetchContent dependencies).
- **Sanitizers clean on every change**: `./scripts/sanitizers.sh`.
- **No new dependencies** without discussion — the current set is GoogleTest,
  Google Benchmark, and pybind11 via FetchContent, nothing else.

## Testing expectations

- New engine behavior: unit tests **plus** differential coverage where feasible
  (`tests/differential/`).
- New strategies/bots: a focused unit test with a fake context (see
  `tests/unit/test_as_requote_gating.cpp` for the pattern), and a tournament
  sanity run showing sane fills/rejects.
- Bug fixes: a regression test that fails before the fix and passes after.
- If you touch the Python bindings, run `python/test_bindings.py` and confirm
  quote parity still holds.

## Pull requests

- Keep PRs focused: one change, one reason.
- Describe *why*, not just *what*. Link the issue or the measurement that
  motivated it.
- Include test results in the PR description (which suites, which presets).
- Don't commit: `build/` trees, `*.so` / `*.o` / `__pycache__`, local data
  captures, or anything your `.gitignore` would exclude. (The GitHub push
  helper used here doesn't honor `.gitignore` — check `git status` yourself.)
- Public-facing text (docs, commit messages, PR descriptions) stays
  professional: no prompt talk, no scaffolding jargon.

## Commit messages

Short imperative summary line, then a body explaining the reasoning:

```
Gate A-S requotes on price change

The arena delivers a book view on nearly every event-loop tick; the bot
was cancelling and re-placing both quotes unconditionally, hammering the
order-rate limiter (8.9M rejections across 50 seeds). Now it only
requotes when desired prices change.
```

## Code of conduct

Be precise, be kind, assume competence. Critique the code, never the person.
Sloppy engineering gets called out and replaced — that's respect, not rudeness.

## License

By contributing, you agree your contributions are licensed under the MIT
License (see [LICENSE](LICENSE)).
