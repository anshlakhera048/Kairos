#!/usr/bin/env python3
"""Generate a static HTML leaderboard from tournament results.

Usage:
    python3 leaderboard.py --input tournament.json --out leaderboard.html

Prefer `kairos arena --html leaderboard.html` (or `python -m kairos.leaderboard`)
when the package is installed. This file is a thin shim over the canonical
implementation in python/kairos/leaderboard.py; it loads that module directly
from the source tree so it keeps working on a bare checkout without building
the compiled extension.
"""

import importlib.util
import sys
from pathlib import Path

_LEADERBOARD_PY = (
    Path(__file__).resolve().parents[2] / "python" / "kairos" / "leaderboard.py"
)


def _load():
    spec = importlib.util.spec_from_file_location(
        "kairos_leaderboard_impl", _LEADERBOARD_PY
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load {_LEADERBOARD_PY}")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    _load().main()


if __name__ == "__main__":
    sys.exit(main())
