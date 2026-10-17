#!/usr/bin/env python3
"""Tests for the season scaffolding (stdlib only, no extension needed).

Run from the repo root:
    python3 python/test_seasons.py
"""

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, "python")

from kairos.seasons import (load_season, read_records, record,
                            season_scores)


def _sample_results():
    return [
        {"bot": "as", "seed": 1000, "fills": 10, "acks": 100,
         "rejects": 0, "position": 2000000, "pnl_lots": 500000,
         "disqualified": False, "dq_reason": ""},
        {"bot": "as", "seed": 1001, "fills": 12, "acks": 100,
         "rejects": 0, "position": -1000000, "pnl_lots": 700000,
         "disqualified": False, "dq_reason": ""},
        {"bot": "fixed", "seed": 1000, "fills": 5, "acks": 50,
         "rejects": 0, "position": 0, "pnl_lots": 100000,
         "disqualified": False, "dq_reason": ""},
    ]


def test_load_season_01():
    cfg = load_season("season-01")
    assert cfg["id"] == "season-01"
    assert cfg["bots"] == ["as", "fixed", "momentum"]
    print("load season-01: OK")


def test_load_season_errors():
    for bad_id, frag in [("no-such-season", "not found")]:
        try:
            load_season(bad_id)
        except ValueError as e:
            assert frag in str(e), e
        else:
            raise AssertionError(f"expected ValueError for {bad_id}")
    with tempfile.TemporaryDirectory() as td:
        p = Path(td) / "bad.json"
        p.write_text(json.dumps({"id": "bad"}))
        try:
            load_season(path=p)
        except ValueError as e:
            assert "missing keys" in str(e), e
        else:
            raise AssertionError("expected ValueError for missing keys")
    print("load errors: OK")


def test_ledger_round_trip():
    with tempfile.TemporaryDirectory() as td:
        ledger = str(Path(td) / "ledger.jsonl")
        n = record(ledger, "season-01", _sample_results())
        assert n == 3
        recs = read_records(ledger)
        assert len(recs) == 3
        assert all(r["season_id"] == "season-01" for r in recs)
        assert all("recorded_at" in r for r in recs)
        assert read_records(ledger, "season-99") == []
        scores = season_scores(
            read_records(ledger, "season-01"),
            {"lambda_inv": 1.0, "lambda_risk": 1.0, "spread_cost": 0.01})
        assert [s["bot"] for s in scores] == ["as", "fixed"]
        assert scores[0]["n_seeds"] == 2
        assert scores[0]["total_fills"] == 22
    print("ledger round-trip: OK")


def test_read_missing_ledger():
    assert read_records("/tmp/definitely-not-here-12345.jsonl") == []
    print("missing ledger: OK")


if __name__ == "__main__":
    test_load_season_01()
    test_load_season_errors()
    test_ledger_round_trip()
    test_read_missing_ledger()
    print("all season tests passed")
