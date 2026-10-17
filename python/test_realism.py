#!/usr/bin/env python3
"""Tests for the realism-gap experiment (needs the compiled extension).

Run from the repo root:
    PYTHONPATH=python python3 python/test_realism.py
"""

import sys

sys.path.insert(0, "python")

from kairos.realism import LEVELS, render_svg, run_gap
from kairos.synth import generate


def test_synth_deterministic():
    n1, t1 = generate("/tmp/synth_a.kai", seconds=30, seed=7)
    n2, t2 = generate("/tmp/synth_b.kai", seconds=30, seed=7)
    assert (n1, t1) == (n2, t2) and n1 > 0 and t1 > 0
    print(f"synth deterministic: OK ({n1} events, {t1} trades)")


def test_gap_schema_and_direction():
    rows, meta = run_gap(seconds=45, seed=7)
    assert [r["level"] for r in rows] == [lv for lv, _, _ in LEVELS]
    for r in rows:
        for k in ("level", "label", "pnl_dollars", "fills", "acks",
                  "fill_rate", "equity_t_s", "equity_dollars"):
            assert k in r, f"missing {k}"
        assert r["fills"] > 0, f"{r['level']} got zero fills"
        assert len(r["equity_t_s"]) == len(r["equity_dollars"]) > 0
    naive, honest = rows[0]["pnl_dollars"], rows[2]["pnl_dollars"]
    assert naive > honest > 0, "naive must overstate honest PnL"
    assert 0 < meta["overstatement_pct"] < 100
    print(f"gap schema: OK (naive ${naive:.2f} vs honest ${honest:.2f} -> "
          f"{meta['overstatement_pct']:.1f}% overstatement)")


def test_gap_deterministic():
    rows1, _ = run_gap(seconds=45, seed=7)
    rows2, _ = run_gap(seconds=45, seed=7)
    for a, b in zip(rows1, rows2):
        assert a["pnl_dollars"] == b["pnl_dollars"], a["level"]
        assert a["fills"] == b["fills"], a["level"]
    print("gap deterministic: OK")


def test_svg_valid():
    rows, meta = run_gap(seconds=30, seed=7)
    svg = render_svg(rows, meta)
    assert svg.startswith("<svg") and "</svg>" in svg
    assert f'{meta["overstatement_pct"]:.1f}%' in svg
    print("svg chart: OK")


if __name__ == "__main__":
    test_synth_deterministic()
    test_gap_schema_and_direction()
    test_gap_deterministic()
    test_svg_valid()
    print("all realism tests passed")
