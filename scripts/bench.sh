#!/usr/bin/env bash
# Build the bench preset and run benchmarks on a pinned core.
# Benchmarks are NEVER run in CI (see docs/benchmarks/README.md).
set -euo pipefail

cmake --preset bench
cmake --build --preset bench

BENCH_BIN="./build/bench/bench/kairos_bench"
if [ ! -x "$BENCH_BIN" ]; then
    echo "benchmark binary not found at $BENCH_BIN" >&2
    exit 1
fi

if command -v taskset >/dev/null 2>&1; then
    echo "running pinned to core 2 (taskset)"
    taskset -c 2 "$BENCH_BIN" "$@"
else
    echo "taskset not found; running unpinned (numbers not publishable)"
    "$BENCH_BIN" "$@"
fi
