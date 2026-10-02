#!/usr/bin/env bash
# Build + test under sanitizers. Must be clean on every change.
set -euo pipefail

for PRESET in asan-ubsan tsan; do
    echo "=== $PRESET ==="
    cmake --preset "$PRESET"
    cmake --build --preset "$PRESET"
    ctest --preset "$PRESET" --output-on-failure
done
echo "sanitizers: clean"
