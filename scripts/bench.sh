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
    if [ -n "${KAIROS_BENCH_CPU:-}" ]; then
        echo "running pinned to core(s) $KAIROS_BENCH_CPU (taskset)"
        taskset -c "$KAIROS_BENCH_CPU" "$BENCH_BIN" "$@"
    else
        echo "KAIROS_BENCH_CPU not set; running unpinned (numbers indicative only)"
        "$BENCH_BIN" "$@"
    fi
else
    echo "taskset not found; running unpinned (numbers not publishable)"
    "$BENCH_BIN" "$@"
fi
