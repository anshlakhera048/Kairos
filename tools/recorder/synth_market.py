#!/usr/bin/env python3
"""Synthetic L2 market data generator for testing and experiments.

Shim: the generator lives in kairos.synth now. Kept so existing scripts
(scripts/realism_gap.sh, python/run_bindings_test.cmake) keep working on a
bare checkout without the compiled extension built.

Usage:
  python3 synth_market.py --out /tmp/synth.kai --seconds 3600 --seed 42
"""

import os
import sys

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "python")
)

from kairos.synth import main  # noqa: E402

if __name__ == "__main__":
    main()
