"""Season scaffolding for the public Kairos bot leaderboard.

A season is a fixed, published configuration: regime, duration, seeds,
eligible bots, scoring weights, budgets. Anyone can reproduce a season
exactly; official results are committed to the repo ledger.

The ledger is an append-only JSONL file: one object per (bot, seed) result,
tagged with season_id and recorded_at. Seasons aggregate with the same
scoring as `kairos arena` (see kairos.leaderboard.compute_scores).

Stdlib-only: works without the compiled extension built.
"""

import json
import time
from pathlib import Path

REQUIRED_KEYS = ("id", "name", "regime", "duration_s", "seeds", "bots",
                 "scoring")


def _repo_root():
    # python/kairos/seasons.py -> repo root.
    return Path(__file__).resolve().parent.parent.parent


def seasons_dir():
    return _repo_root() / "data" / "seasons"


def default_ledger_path():
    """Repo ledger for official seasons; ~/.kairos for ad-hoc runs."""
    repo_ledger = _repo_root() / "data" / "leaderboard" / "ledger.jsonl"
    if (_repo_root() / "pyproject.toml").exists():
        return repo_ledger
    return Path.home() / ".kairos" / "ledger.jsonl"


def list_seasons():
    d = seasons_dir()
    if not d.is_dir():
        return []
    out = []
    for p in sorted(d.glob("*.json")):
        try:
            out.append(load_season(path=p))
        except ValueError:
            continue
    return out


def load_season(season_id=None, path=None):
    """Load and validate a season config. Raises ValueError on problems."""
    if path is None:
        if season_id is None:
            raise ValueError("give a season id or a config path")
        path = seasons_dir() / f"{season_id}.json"
    p = Path(path)
    if not p.is_file():
        raise ValueError(f"season config not found: {p}")
    try:
        cfg = json.loads(p.read_text())
    except json.JSONDecodeError as e:
        raise ValueError(f"invalid JSON in {p}: {e}") from e
    missing = [k for k in REQUIRED_KEYS if k not in cfg]
    if missing:
        raise ValueError(f"{p} missing keys: {', '.join(missing)}")
    if not isinstance(cfg["bots"], list) or not cfg["bots"]:
        raise ValueError(f"{p}: 'bots' must be a non-empty list")
    if not isinstance(cfg["seeds"], int) or cfg["seeds"] < 1:
        raise ValueError(f"{p}: 'seeds' must be a positive integer")
    return cfg


def record(ledger_path, season_id, results):
    """Append tournament results to the ledger. Returns records written."""
    ledger_path = Path(ledger_path)
    ledger_path.parent.mkdir(parents=True, exist_ok=True)
    now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    n = 0
    with open(ledger_path, "a") as f:
        for r in results:
            rec = {"season_id": season_id, "recorded_at": now,
                   "bot": r["bot"], "seed": r["seed"],
                   "fills": r["fills"], "acks": r["acks"],
                   "rejects": r["rejects"], "position": r["position"],
                   "pnl_lots": r["pnl_lots"],
                   "disqualified": r["disqualified"],
                   "dq_reason": r.get("dq_reason", "")}
            f.write(json.dumps(rec) + "\n")
            n += 1
    return n


def read_records(ledger_path, season_id=None):
    """Read ledger records, optionally filtered to one season."""
    p = Path(ledger_path)
    if not p.is_file():
        return []
    out = []
    with open(p) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            if season_id is None or rec.get("season_id") == season_id:
                out.append(rec)
    return out


def season_scores(records, scoring):
    """Aggregate a season's records into ranked scores."""
    from .leaderboard import compute_scores
    return compute_scores(records, **scoring)
