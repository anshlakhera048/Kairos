#!/usr/bin/env python3
"""Analyze realism-gap experiment results.

Reads the JSON lines from kairos_realism_gap and the capture file,
computes metrics, and generates figures.

Metrics per run:
  - Total PnL (realized + unrealized at final mid)
  - Sharpe-like: mean/std of per-minute equity changes, annualized
  - Block bootstrap 95% CI for PnL
  - Fill rate, inventory stats
  - Markouts: post-fill mid-price move at 1s, 10s, 60s (adverse selection)

Usage:
  python3 analyze.py --results /tmp/gap_results.jsonl --capture /tmp/synth_exp.kai --out /tmp/gap_figs/
"""

import argparse
import json
import math
import os
import random
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "../recorder"))
from format import Reader


def load_mids(capture_path):
    """Extract (ts_ns, mid_dollars) series from the capture."""
    r = Reader(capture_path)
    bids = {}
    asks = {}
    mids = []
    for e in r.events():
        t = e["type"]
        if t == 0:  # snapshot
            bids.clear()
            asks.clear()
            for p, q, s in e["levels"]:
                (bids if s == 0 else asks)[p] = q
        elif t == 1:  # diff
            for p, q, s in e["levels"]:
                d = bids if s == 0 else asks
                if q == 0:
                    d.pop(p, None)
                else:
                    d[p] = q
        if bids and asks:
            bb = max(bids)
            ba = min(asks)
            if bb < ba:
                mids.append((e["exchange_ts_ns"], (bb + ba) / 2.0))
    r.close()
    return mids


def block_bootstrap_ci(values, n_boot=1000, block=5, seed=42):
    """Block bootstrap 95% CI for the mean."""
    rng = random.Random(seed)
    n = len(values)
    if n < block * 2:
        return (float("nan"), float("nan"))
    means = []
    for _ in range(n_boot):
        sample = []
        while len(sample) < n:
            start = rng.randint(0, n - block)
            sample.extend(values[start:start + block])
        sample = sample[:n]
        means.append(sum(sample) / n)
    means.sort()
    return (means[int(0.025 * n_boot)], means[int(0.975 * n_boot)])


def analyze_run(run, mids):
    fills = run["fills"]
    equity = run["equity"]
    # PnL in dollars (quote lots -> dollars: / (qty_scale * price_scale))
    # qty_scale=1e6, price_scale=100 for synthetic.
    to_dollars = 1.0 / (1e6 * 100)

    # Per-minute equity for Sharpe and bootstrap.
    eq_by_min = {}
    for pt in equity:
        minute = pt["t"] // 60_000_000_000
        eq_by_min[minute] = pt["e"] * to_dollars
    minutes = sorted(eq_by_min)
    # Total PnL = final equity - initial equity.
    pnl = eq_by_min[minutes[-1]] - eq_by_min[minutes[0]] if minutes else 0.0
    rets = []
    for i in range(1, len(minutes)):
        rets.append(eq_by_min[minutes[i]] - eq_by_min[minutes[i - 1]])
    sharpe = 0.0
    if len(rets) > 1:
        mean = sum(rets) / len(rets)
        var = sum((x - mean) ** 2 for x in rets) / (len(rets) - 1)
        if var > 0:
            # Annualized Sharpe (per-minute -> per-year).
            sharpe = mean / math.sqrt(var) * math.sqrt(525600)

    # Bootstrap CI for total PnL (sum of per-minute changes).
    ci_lo, ci_hi = block_bootstrap_ci(rets)
    ci_lo *= len(rets)
    ci_hi *= len(rets)

    # Fill rate.
    fill_rate = len(fills) / run["acks"] if run["acks"] else 0.0

    # Markouts: for each fill, mid-price move after h seconds.
    # Build a mid-price lookup (binary search).
    mid_ts = [t for t, _ in mids]
    mid_px = [p for _, p in mids]
    horizons = [1, 10, 60]
    markouts = {h: [] for h in horizons}
    for f in fills:
        ft = f["t"]
        fp_dollars = f["p"] / 100.0  # ticks -> dollars (price_scale=100)
        # Find mid at fill time.
        import bisect
        i = bisect.bisect_right(mid_ts, ft) - 1
        if i < 0:
            continue
        # For each horizon, find mid at ft + h.
        for h in horizons:
            j = bisect.bisect_right(mid_ts, ft + h * 1_000_000_000) - 1
            if j <= i:
                continue
            # Markout: favorable move is positive.
            # If I bought (bid), I want price to go up.
            # If I sold (ask), I want price to go down.
            move = mid_px[j] - fp_dollars
            if not f["bid"]:
                move = -move
            markouts[h].append(move)

    avg_markout = {h: (sum(v) / len(v) if v else 0.0)
                   for h, v in markouts.items()}

    return {
        "run": run["run"],
        "pnl_dollars": pnl,
        "sharpe": sharpe,
        "pnl_ci": (ci_lo, ci_hi),
        "n_fills": len(fills),
        "fill_rate": fill_rate,
        "final_pos_lots": run["final_position"] / 1e6,
        "markout_1s": avg_markout[1],
        "markout_10s": avg_markout[10],
        "markout_60s": avg_markout[60],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--capture", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    print("loading mids...", flush=True)
    mids = load_mids(args.capture)
    print(f"  {len(mids)} mid points", flush=True)

    results = []
    for line in open(args.results):
        run = json.loads(line)
        results.append(analyze_run(run, mids))

    # Print table.
    print(f"\n{'run':<18} {'PnL($)':>10} {'Sharpe':>8} {'fills':>7} "
          f"{'fill%':>6} {'mkout1s':>8} {'mkout60s':>9}")
    print("-" * 80)
    for r in results:
        print(f"{r['run']:<18} {r['pnl_dollars']:>10.2f} {r['sharpe']:>8.2f} "
              f"{r['n_fills']:>7} {r['fill_rate']*100:>5.1f}% "
              f"{r['markout_1s']:>8.4f} {r['markout_60s']:>9.4f}")
        print(f"  95% CI for PnL: (${r['pnl_ci'][0]:.2f}, ${r['pnl_ci'][1]:.2f})")

    # Save JSON for the research note.
    with open(os.path.join(args.out, "metrics.json"), "w") as f:
        json.dump(results, f, indent=2)
    print(f"\nwrote {args.out}/metrics.json", flush=True)

    # Figures (matplotlib, if available).
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt

        # PnL by realism level.
        fig, ax = plt.subplots(figsize=(8, 5))
        names = [r["run"] for r in results]
        pnls = [r["pnl_dollars"] for r in results]
        cis = [r["pnl_ci"] for r in results]
        lo = [p - c[0] for p, c in zip(pnls, cis)]
        hi = [c[1] - p for p, c in zip(pnls, cis)]
        ax.bar(names, pnls, yerr=[lo, hi], capsize=5)
        ax.set_ylabel("PnL ($)")
        ax.set_title("PnL by realism level (95% block-bootstrap CI)")
        plt.xticks(rotation=15)
        plt.tight_layout()
        plt.savefig(os.path.join(args.out, "pnl_by_realism.png"), dpi=150)
        plt.close()

        # Markouts.
        fig, ax = plt.subplots(figsize=(8, 5))
        x = range(len(names))
        width = 0.25
        for i, h in enumerate([1, 10, 60]):
            vals = [r[f"markout_{h}s"] for r in results]
            ax.bar([p + i * width for p in x], vals, width,
                   label=f"{h}s")
        ax.set_xticks([p + width for p in x])
        ax.set_xticklabels(names, rotation=15)
        ax.set_ylabel("Avg markout ($)")
        ax.set_title("Adverse selection: post-fill markouts")
        ax.legend()
        plt.tight_layout()
        plt.savefig(os.path.join(args.out, "markouts.png"), dpi=150)
        plt.close()
        print(f"wrote figures to {args.out}/", flush=True)
    except ImportError:
        print("matplotlib not available; skipping figures", flush=True)


if __name__ == "__main__":
    main()
