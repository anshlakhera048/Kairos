#!/usr/bin/env python3
"""Generate a static HTML leaderboard from tournament results.

Usage:
    python3 leaderboard.py --input tournament.json --out leaderboard.html

The HTML is self-contained (no backend, no external JS). It includes:
- Ranked table by total score (PnL - lambda*inventory + lambda*risk)
- Per-bot details: fills, PnL, position, disqualifications
"""

import argparse
import json
import math
import os


def compute_scores(results, lambda_inv=1.0, lambda_risk=1.0, spread_cost=0.01):
    """Aggregate per-seed results into scores."""
    by_bot = {}
    for r in results:
        by_bot.setdefault(r["bot"], []).append(r)

    scores = []
    for bot, rs in by_bot.items():
        pnls = [r["pnl_lots"] / 1e6 for r in rs]  # lots -> dollars
        mean_pnl = sum(pnls) / len(pnls)
        # Inventory penalty: mean |position| * spread cost (simplified).
        mean_pos = sum(abs(r["position"]) / 1e6 for r in rs) / len(rs)
        penalty = mean_pos * spread_cost
        # Risk-adjusted: Sharpe-like.
        risk = 0.0
        if len(pnls) > 1:
            var = sum((p - mean_pnl) ** 2 for p in pnls) / (len(pnls) - 1)
            if var > 0:
                risk = mean_pnl / math.sqrt(var)
        total = mean_pnl - lambda_inv * penalty + lambda_risk * risk
        scores.append({
            "bot": bot,
            "n_seeds": len(rs),
            "mean_pnl": mean_pnl,
            "penalty": penalty,
            "risk": risk,
            "total": total,
            "total_fills": sum(r["fills"] for r in rs),
            "disqualified": sum(1 for r in rs if r["disqualified"]),
        })
    scores.sort(key=lambda s: s["total"], reverse=True)
    return scores


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--input", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    with open(args.input) as f:
        results = json.load(f)

    scores = compute_scores(results)

    html = """<!DOCTYPE html>
<html><head><meta charset="utf-8"><title>Kairos Arena Leaderboard</title>
<style>
body { font-family: system-ui, sans-serif; max-width: 900px; margin: 40px auto; padding: 0 20px; }
table { border-collapse: collapse; width: 100%; }
th, td { border: 1px solid #ddd; padding: 8px 12px; text-align: right; }
th { background: #f5f5f5; }
td:first-child, th:first-child { text-align: left; }
tr.dq { background: #fff0f0; }
h1 { font-size: 1.5em; }
.note { color: #666; font-size: 0.9em; }
</style></head><body>
<h1>Kairos Arena Leaderboard</h1>
<p class="note">Score = mean PnL &minus; inventory penalty + risk-adjusted term.
Higher is better. Disqualified bots are highlighted.</p>
<table><tr>
<th>Rank</th><th>Bot</th><th>Score</th><th>Mean PnL ($)</th>
<th>Inv. Penalty</th><th>Risk (Sharpe)</th><th>Fills</th><th>Seeds</th><th>DQ</th>
</tr>
"""
    for i, s in enumerate(scores, 1):
        dq_cls = ' class="dq"' if s["disqualified"] else ""
        html += (f'<tr{dq_cls}><td>{i}</td><td>{s["bot"]}</td>'
                 f'<td>{s["total"]:.2f}</td><td>{s["mean_pnl"]:.2f}</td>'
                 f'<td>{s["penalty"]:.2f}</td><td>{s["risk"]:.2f}</td>'
                 f'<td>{s["total_fills"]}</td><td>{s["n_seeds"]}</td>'
                 f'<td>{s["disqualified"]}</td></tr>\n')
    html += "</table></body></html>\n"

    with open(args.out, "w") as f:
        f.write(html)
    print(f"wrote {args.out}")


if __name__ == "__main__":
    main()
