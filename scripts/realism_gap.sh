#!/usr/bin/env bash
# Realism-gap experiment: one command regenerates every figure and table.
#
# Pipeline:
#   1. Generate synthetic L2 capture (fixed seed).
#   2. Build the experiment driver.
#   3. Run the 4 realism levels.
#   4. Analyze and generate figures.
#
# Usage: scripts/realism_gap.sh [output_dir]
# Output: <output_dir>/metrics.json, pnl_by_realism.png, markouts.png
set -euo pipefail

OUT_DIR="${1:-./docs/research/figures}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DATA="/tmp/kairos_realism_gap.kai"
RESULTS="/tmp/kairos_realism_gap.jsonl"

echo "=== 1/4 generating synthetic capture (seed 7) ==="
python3 "$REPO_ROOT/tools/recorder/synth_market.py" \
    --out "$DATA" --seconds 1800 --seed 7 --trade-rate 3.0

echo "=== 2/4 building experiment driver ==="
cmake --build --preset debug --target kairos_realism_gap

echo "=== 3/4 running 4 realism levels ==="
"$REPO_ROOT/build/debug/tools/kairos_realism_gap" "$DATA" > "$RESULTS"
wc -l "$RESULTS"

echo "=== 4/4 analyzing ==="
python3 "$REPO_ROOT/tools/realism_gap/analyze.py" \
    --results "$RESULTS" --capture "$DATA" --out "$OUT_DIR"

echo "done: $OUT_DIR"
