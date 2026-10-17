# ADR 0006 — `kairos realism-gap` one-command demo

Date: 2026-10-18
Status: accepted

## Context

The realism-gap finding (naive fill-at-touch overstates market-maker PnL
by ~19%, docs/research/realism-gap.md) was reproducible only via
`scripts/realism_gap.sh`: build the C++ experiment driver, generate a
capture, run, analyze. Too much friction for the finding to travel — it is
the single most shareable result Kairos has, and it needed one command.

## Decision

1. **`kairos realism-gap`**: generates a synthetic L2 capture and runs
   the Python A-S market maker under L1–L4 (naive / +latency /
   +risk-averse queue / +probabilistic queue) via the existing
   `kairos.Simulator` queue models. Prints the comparison table with the
   overstatement %, plus `--out` JSON and `--chart` (self-contained SVG
   equity curves, stdlib-only).
2. **Format helpers move into the package.** The `.kai` Writer/Reader
   moved from `tools/recorder/format.py` to `kairos/_kai_format.py`;
   `tools/recorder/format.py` is now a shim. The synth generator moved
   from `tools/recorder/synth_market.py` to `kairos/synth.generate()`;
   the tool is a shim. Both shims work on a bare checkout (no extension
   built) via a `python/` sys.path insert — the same pattern as the
   leaderboard shim.
3. **Lazy extension import.** `kairos/__init__.py` no longer imports
   `kairos_py` at package import time; extension names resolve on first
   use with a clear error ("run ./scripts/build.sh or pip install
   kairos-engine"). This keeps pure-Python submodules (synth, leaderboard,
   _kai_format) usable without a build.

## Consequences

- The headline finding is one command: `kairos realism-gap`.
- `scripts/realism_gap.sh` (C++ driver) remains the heavyweight,
  fully-instrumented experiment; the Python demo trades some
  instrumentation for zero setup. Both are deterministic.
- Honesty note, printed by the command: synthetic data — the relative
  gap is the finding; absolute PnL is not tradable.

## Alternatives considered

- **Vendoring the Writer into kairos/synth.py** — rejected: duplicating
  a serialization format invites drift; the move + shim keeps one
  canonical implementation.
- **Shelling out to tools/recorder/synth_market.py from the CLI** —
  rejected: the script is not shipped in the wheel, so pip-installed
  users would get a broken command.
