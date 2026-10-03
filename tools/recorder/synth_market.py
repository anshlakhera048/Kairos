#!/usr/bin/env python3
"""Synthetic L2 market data generator for testing and experiments.

Generates a realistic limit order book feed with:
  - Random-walk mid price (configurable volatility)
  - L2 depth (N levels per side, quantities drawn from a distribution)
  - Trades as a Poisson process, mostly at the touch (distance from mid
    drawn from an exponential, mimicking the A-S kappa assumption)

Output: Kairos binary format v1 (see docs/design/data-format.md).

Usage:
  python3 synth_market.py --out /tmp/synth.kai --seconds 3600 --seed 42
"""

import argparse
import math
import random
import sys
import os

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from format import Writer, BID, ASK


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--seconds", type=float, default=600)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--mid0", type=float, default=100.0,
                    help="initial mid price")
    ap.add_argument("--sigma", type=float, default=0.0002,
                    help="vol per sqrt(sec), relative")
    ap.add_argument("--levels", type=int, default=10,
                    help="L2 levels per side")
    ap.add_argument("--tick", type=float, default=0.01,
                    help="tick size")
    ap.add_argument("--trade-rate", type=float, default=2.0,
                    help="trades per second (Poisson)")
    ap.add_argument("--kappa", type=float, default=2.0,
                    help="trade distance decay (per tick)")
    args = ap.parse_args()

    rng = random.Random(args.seed)
    w = Writer(args.out, "SYNTH-USD", "synth", 100, 1_000_000)

    mid = args.mid0
    t_ns = 1_000_000_000
    seq = 1
    end_ns = t_ns + int(args.seconds * 1e9)

    # Book state: dict price -> qty for bids and asks.
    bids = {}
    asks = {}

    def rebuild_book():
        bids.clear()
        asks.clear()
        for i in range(args.levels):
            bp = round(mid - (i + 1) * args.tick, 2)
            ap_ = round(mid + (i + 1) * args.tick, 2)
            bids[bp] = rng.uniform(0.5, 5.0)
            asks[ap_] = rng.uniform(0.5, 5.0)

    def emit_snapshot():
        nonlocal seq
        w.snapshot(seq,
                   sorted(bids.items(), reverse=True),
                   sorted(asks.items()),
                   exchange_ts_ns=t_ns)
        seq += 1

    rebuild_book()
    emit_snapshot()

    # Track the previous book to emit removals.
    prev_bids = dict(bids)
    prev_asks = dict(asks)

    # Next trade time (Poisson).
    next_trade_ns = t_ns + int(rng.expovariate(args.trade_rate) * 1e9)
    # Next book update (every ~200ms, random walk step).
    next_book_ns = t_ns + int(0.2 * 1e9)

    n_trades = 0
    while t_ns < end_ns:
        t_ns = min(next_trade_ns, next_book_ns)
        if t_ns >= end_ns:
            break
        if t_ns == next_book_ns:
            # Random-walk the mid; rebuild the book around it.
            dt = 0.2
            mid *= math.exp(args.sigma * math.sqrt(dt) * rng.gauss(0, 1))
            mid = round(mid, 2)
            rebuild_book()
            next_book_ns = t_ns + int(0.2 * 1e9)
            # Emit a diff: new/changed levels plus removals for stale ones.
            changes = []
            for p, q in bids.items():
                changes.append((BID, p, q))
            for p, q in asks.items():
                changes.append((ASK, p, q))
            for p in prev_bids:
                if p not in bids:
                    changes.append((BID, p, 0.0))
            for p in prev_asks:
                if p not in asks:
                    changes.append((ASK, p, 0.0))
            prev_bids = dict(bids)
            prev_asks = dict(asks)
            w.diff(seq, changes, exchange_ts_ns=t_ns)
            seq += 1
        else:
            # Trade: pick a side, distance from touch via exponential.
            side = BID if rng.random() < 0.5 else ASK  # taker side
            dist_ticks = int(rng.expovariate(args.kappa))  # 0, 1, 2, ...
            if side == BID:
                # Buyer-initiated: lifts the ask.
                px = round(mid + (dist_ticks + 1) * args.tick, 2)
                qty = rng.uniform(0.1, 2.0)
                # Reduce the ask level (or the nearest).
                if px in asks:
                    asks[px] = max(0.0, asks[px] - qty)
                    new_q = asks[px]
                    if new_q == 0:
                        del asks[px]
                    # Emit the level change so the book stays in sync.
                    w.diff(seq, [(ASK, px, new_q)], exchange_ts_ns=t_ns)
                    seq += 1
            else:
                px = round(mid - (dist_ticks + 1) * args.tick, 2)
                qty = rng.uniform(0.1, 2.0)
                if px in bids:
                    bids[px] = max(0.0, bids[px] - qty)
                    new_q = bids[px]
                    if new_q == 0:
                        del bids[px]
                    w.diff(seq, [(BID, px, new_q)], exchange_ts_ns=t_ns)
                    seq += 1
            w.trade(seq, side, px, qty, exchange_ts_ns=t_ns)
            seq += 1
            n_trades += 1
            next_trade_ns = t_ns + int(rng.expovariate(args.trade_rate) * 1e9)

    n = w.close()
    print(f"wrote {n} events ({n_trades} trades) to {args.out}")


if __name__ == "__main__":
    main()
