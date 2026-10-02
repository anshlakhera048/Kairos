#!/usr/bin/env bash
# Configure + build a preset. Usage: ./scripts/build.sh [preset]   (default: debug)
set -euo pipefail
PRESET="${1:-debug}"
cmake --preset "$PRESET"
cmake --build --preset "$PRESET"
