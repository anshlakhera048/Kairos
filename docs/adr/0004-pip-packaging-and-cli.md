# ADR 0004 — pip packaging (`kairos-engine`) and the `kairos` CLI

Date: 2026-10-17
Status: accepted

## Context

Kairos's engine, backtester, and tournament arena are C++ behind a CMake
build. That is correct for the hot path, but it is a wall for everyone else:
a quant researcher, student, or agent-builder who wants to try the arena must
install a C++20 toolchain, configure CMake, and find the right binaries before
seeing a single fill. Every viral developer tool in our space installs in one
line and shows value in under five minutes. Kairos did neither.

## Decision

1. Ship a `kairos-engine` wheel built by **scikit-build-core** (build-time
   only; the only new runtime dependency declared is `numpy`, which the
   Python API already required — it returns numpy arrays).
2. Install a `kairos` console script with two subcommands:
   - `kairos arena` — runs the `kairos_tournament` binary shipped inside the
     wheel (`kairos/bin/`), parses its JSON, prints a ranked table.
   - `kairos demo` — a ~30-second, 10-seed arena run. The two-minute wow.
3. Keep the CMake developer workflow untouched: the pybind11 module still
   builds in-source for `ctest`, tests/benchmarks stay available, and wheel
   builds disable `KAIROS_BUILD_TESTS`/`KAIROS_BUILD_BENCHMARKS` purely to
   skip the googletest/benchmark FetchContent downloads during `pip install`.
4. The pre-existing `tools/arena/leaderboard.py` becomes a thin shim over the
   canonical `kairos.leaderboard` module (scoring + HTML), loaded without
   importing the compiled extension so it keeps working on a bare checkout.

## Consequences

- `pip install kairos-engine && kairos demo` is the new front door. The
  CMake path remains the contributor path.
- Wheel builds need a C++20 compiler and CMake on the installer's machine
  until prebuilt wheels are published via cibuildwheel (future work; needs
  CI, currently parked).
- Version lives in `pyproject.toml`; `kairos --version` reads it from
  installed metadata with a static fallback.

## Alternatives considered

- **Pure `setup.py` with manual compiler invocation** — rejected: fragile
  across platforms, reinvents what scikit-build-core does.
- **Shipping only the Python backtester, not the arena binary** — rejected:
  the arena is the differentiator; a wheel without it is a weaker product.
- **Reimplementing the tournament driver in Python over new arena
  bindings** — deferred: correct long-term (custom bots in the CLI), but a
  large C++ API surface. The binary-shim gets us the 2-minute wow now.
