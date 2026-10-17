"""The realism gap: same strategy, same data, honest fills vs naive fills.

A naive backtest assumes limit orders fill the moment a trade prints at
their price ("fill at touch") with zero latency. A realistic simulation
models feed/order latency and queue position. This experiment runs the
same Avellaneda-Stoikov market maker on the same synthetic L2 capture
under four realism levels and measures how much PnL the naive assumption
overstates.

Levels (following docs/research/realism-gap.md):
  L1  naive: fill at touch, zero latency
  L2  L1 + 5ms feed / 2ms order latency
  L3  L2 + risk-averse queue (only trades advance my queue position)
  L4  L2 + probabilistic queue (Binomial(0.5) cancel attribution)

Deterministic: fixed synth seed + fixed simulator seed -> identical numbers.
"""

import tempfile
from pathlib import Path

from . import Simulator
from .as_strategy import AvellanedaStoikov
from .synth import generate

# equity_lots -> dollars: price in ticks (1/100 $) x qty in lots (1e-6).
LOTS_TO_DOLLARS = 1e8

LEVELS = [
    ("L1", "naive: fill at touch, zero latency",
     dict(feed_latency_ns=0, order_latency_ns=0, queue_model="naive")),
    ("L2", "L1 + 5ms feed / 2ms order latency",
     dict(feed_latency_ns=5_000_000, order_latency_ns=2_000_000,
          queue_model="naive")),
    ("L3", "L2 + risk-averse queue",
     dict(feed_latency_ns=5_000_000, order_latency_ns=2_000_000,
          queue_model="risk_averse")),
    ("L4", "L2 + probabilistic queue",
     dict(feed_latency_ns=5_000_000, order_latency_ns=2_000_000,
          queue_model="probabilistic", p_ahead=0.5)),
]


def run_gap(capture_path=None, seconds=600, seed=7,
            strategy_factory=AvellanedaStoikov,
            timer_interval_ns=500_000_000, progress=None):
    """Run the four realism levels. Returns (rows, meta).

    rows: list of dicts with level/label/pnl_dollars/fills/acks/
    fill_rate/equity_t_s/equity_dollars.
    """
    tmpdir = None
    if capture_path is None:
        tmpdir = tempfile.TemporaryDirectory()
        capture_path = str(Path(tmpdir.name) / "realism.kai")
        n_events, n_trades = generate(capture_path, seconds=seconds,
                                      seed=seed, trade_rate=3.0)
    else:
        n_events = n_trades = None

    rows = []
    try:
        for level, label, kw in LEVELS:
            if progress is not None:
                progress(level)
            sim = Simulator(maker_fee_bp=-10, taker_fee_bp=30,
                            seed=0x9E3779B97F4A7C15, **kw)
            res = sim.run(capture_path, strategy_factory(),
                          timer_interval_ns=timer_interval_ns)
            st = res["stats"]
            eq = res["equity"]
            pnl = float(eq["equity_lots"][-1]) / LOTS_TO_DOLLARS
            fills = int(st["fills"])
            acks = int(st["acks"])
            rows.append({
                "level": level,
                "label": label,
                "pnl_dollars": pnl,
                "fills": fills,
                "acks": acks,
                "fill_rate": fills / acks if acks else 0.0,
                "equity_t_s": [float(t) / 1e9 for t in eq["t_ns"]],
                "equity_dollars": [float(e) / LOTS_TO_DOLLARS
                                   for e in eq["equity_lots"]],
            })
    finally:
        if tmpdir is not None:
            tmpdir.cleanup()

    naive = rows[0]["pnl_dollars"]
    honest = rows[2]["pnl_dollars"]
    overstatement = ((naive - honest) / naive) if naive else 0.0
    meta = {
        "seconds": seconds,
        "seed": seed,
        "n_events": n_events,
        "n_trades": n_trades,
        "overstatement_pct": 100.0 * overstatement,
    }
    return rows, meta


def render_svg(rows, meta, width=720, height=380):
    """Self-contained SVG with the L1 vs L3 equity curves (stdlib only)."""
    curves = [(r["level"], r["equity_t_s"], r["equity_dollars"]) for r in rows]
    colors = {"L1": "#d62728", "L2": "#ff7f0e", "L3": "#1f77b4",
              "L4": "#2ca02c"}
    labels = {r["level"]: r["label"] for r in rows}

    all_t = [t for _, ts, _ in curves for t in ts]
    all_y = [y for _, _, ys in curves for y in ys]
    t0, t1 = min(all_t), max(all_t)
    y0, y1 = min(all_y), max(all_y)
    if y1 == y0:
        y1 = y0 + 1
    pad = (y1 - y0) * 0.08
    y0, y1 = y0 - pad, y1 + pad

    ml, mr, mt, mb = 64, 16, 28, 44
    pw, ph = width - ml - mr, height - mt - mb

    def X(t):
        return ml + (t - t0) / (t1 - t0) * pw if t1 > t0 else ml

    def Y(v):
        return mt + (1 - (v - y0) / (y1 - y0)) * ph

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" '
        f'height="{height}" font-family="system-ui,sans-serif">',
        f'<rect width="{width}" height="{height}" fill="#ffffff"/>',
        f'<text x="{ml}" y="18" font-size="15" font-weight="bold" '
        f'fill="#222">Realism gap: naive vs honest fills</text>',
        f'<text x="{width - mr}" y="18" font-size="11" fill="#666" '
        f'text-anchor="end">naive overstates PnL by '
        f'{meta["overstatement_pct"]:.1f}%</text>',
    ]
    # Axes.
    parts.append(
        f'<line x1="{ml}" y1="{mt + ph}" x2="{ml + pw}" y2="{mt + ph}" '
        f'stroke="#999"/>')
    parts.append(
        f'<line x1="{ml}" y1="{mt}" x2="{ml}" y2="{mt + ph}" stroke="#999"/>')
    for i in range(5):
        v = y0 + (y1 - y0) * i / 4
        y = Y(v)
        parts.append(
            f'<line x1="{ml}" y1="{y}" x2="{ml + pw}" y2="{y}" '
            f'stroke="#eee"/>')
        parts.append(
            f'<text x="{ml - 8}" y="{y + 4}" font-size="10" fill="#666" '
            f'text-anchor="end">${v:,.0f}</text>')
    parts.append(
        f'<text x="{ml + pw / 2}" y="{height - 8}" font-size="11" '
        f'fill="#666" text-anchor="middle">time (s)</text>')

    for level, ts, ys in curves:
        pts = " ".join(f"{X(t):.1f},{Y(y):.1f}" for t, y in zip(ts, ys))
        color = colors.get(level, "#333")
        width_w = 2.5 if level in ("L1", "L3") else 1.2
        opacity = 1.0 if level in ("L1", "L3") else 0.55
        parts.append(
            f'<polyline points="{pts}" fill="none" stroke="{color}" '
            f'stroke-width="{width_w}" opacity="{opacity}"/>')

    # Legend.
    lx = ml + 8
    for i, (level, _, _) in enumerate(curves):
        y = mt + 10 + i * 18
        parts.append(
            f'<rect x="{lx}" y="{y - 9}" width="14" height="3" '
            f'fill="{colors.get(level, "#333")}"/>')
        parts.append(
            f'<text x="{lx + 20}" y="{y}" font-size="11" fill="#333">'
            f'{level}: {labels[level]}</text>')

    parts.append("</svg>")
    return "\n".join(parts) + "\n"
