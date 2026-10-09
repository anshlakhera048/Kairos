#!/usr/bin/env bash
# Full differential soak: engine vs naive reference over ~18M adversarial
# ops (the figure cited in the README). This is the long version of the
# `differential_*` ctest entries, which run a 400K-op smoke per mode.
#
# Usage: ./scripts/diff_soak.sh [build_dir]   (default: build/debug)
set -euo pipefail

BUILD_DIR="${1:-build/debug}"
BIN="$BUILD_DIR/tests/kairos_diff_test"
if [[ ! -x "$BIN" ]]; then
    echo "error: $BIN not found; build the debug preset first" >&2
    exit 1
fi

# 4 modes x 1.5M ops x 3 seeds = 18M ops, bit-identical event streams required.
for mode in balanced heavy-cancels deep-sweeps tight; do
    echo "=== mode=$mode ==="
    "$BIN" --ops 1500000 --seeds 3 --mode "$mode"
done
echo "diff soak: 18M ops, no divergence"
