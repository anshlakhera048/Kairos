#!/usr/bin/env bash
# Format check/apply for all C++ sources.
# Usage: ./scripts/format.sh [--check]   (default: --check)
set -euo pipefail

MODE="${1:---check}"
FILES=$(find include src tests bench tools \( -name '*.hpp' -o -name '*.cpp' \) | sort)

if ! command -v clang-format >/dev/null 2>&1; then
    echo "clang-format not found; skipping format ${MODE}."
    exit 0
fi

if [ "$MODE" = "--check" ]; then
    clang-format --dry-run --Werror $FILES
    echo "format: clean"
else
    clang-format -i $FILES
    echo "format: applied"
fi
