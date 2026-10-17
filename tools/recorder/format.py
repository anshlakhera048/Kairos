"""Shim: the .kai format helpers live in kairos._kai_format now.

Kept so the recorder tooling (recorder.py, analyze.py, synth_market.py)
keeps working on a bare checkout without the compiled extension built.
New code should import from kairos._kai_format directly.
"""

import os
import sys

sys.path.insert(
    0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "python")
)

from kairos._kai_format import (  # noqa: E402
    ASK,
    BID,
    DIFF,
    EVENT_STRUCT,
    HEADER_SIZE,
    HEADER_STRUCT,
    LEVEL_STRUCT,
    MAGIC,
    SNAPSHOT,
    TRADE,
    VERSION,
    Reader,
    Writer,
)

__all__ = [
    "ASK", "BID", "DIFF", "EVENT_STRUCT", "HEADER_SIZE", "HEADER_STRUCT",
    "LEVEL_STRUCT", "MAGIC", "SNAPSHOT", "TRADE", "VERSION", "Reader", "Writer",
]
