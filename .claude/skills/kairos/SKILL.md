---
name: kairos
description: Build, backtest, and compete market-making bots on Kairos, a real deterministic C++ matching engine. Use when the user wants a trading bot, a backtest with honest fill modeling, or to enter the Kairos bot leaderboard.
---

# Kairos — market-making bots on a real matching engine

Kairos is a deterministic limit order book matching engine (C++, integer
ticks/lots, no randomness in the engine). You write a bot, run it against
synthetic or recorded market data, and get honest fills — queue position,
feed/order latency, and adverse selection are modeled, not assumed.

Install:

```bash
pip install kairos-hft
kairos demo   # ~30s: 10-seed tournament on the real engine. Start here.
```

## Write a bot (Python)

Subclass `kairos.agents.ArenaBot` and override the callbacks you need:

```python
from kairos.agents import ArenaBot, snapshot

class MyMaker(ArenaBot):
    def on_timer(self, ctx):
        s = snapshot(ctx)  # dict: mid_px, spread_ticks, bid/ask px+qty, position_lots, ...
        if s["position_lots"] == 0:
            self.quote(ctx, s["bid_px"] - 1, s["ask_px"] + 1, qty_lots=10)

    def quote(self, ctx, bid_px, ask_px, qty_lots):
        for oid in list(self.live):
            ctx.cancel(oid)
        self.live = [
            ctx.send_limit("bid", bid_px, qty_lots, post_only=True),
            ctx.send_limit("ask", ask_px, qty_lots, post_only=True),
        ]

    def __init__(self):
        self.live = []

    def on_ack(self, ctx, ack):
        self.live.append(ack["order_id"])  # if quoting outside on_timer
```

Callback reference (`ctx` is the per-callback context):

- `on_book_update(ctx)` — every book tick. `ctx.book()` → delayed L2 view
  (no lookahead, ever). Prefer `on_timer` for decisions; this fires a lot.
- `on_trade(ctx, trade)`, `on_own_fill(ctx, fill)`,
  `on_ack(ctx, ack)`, `on_reject(ctx, order_id)`,
  `on_cancel(ctx, order_id)`, `on_timer(ctx)`.
- Orders: `ctx.send_limit(side, price_ticks, qty_lots, post_only)`,
  `ctx.cancel(order_id)`. `ctx.now_ns` → your logical timestamp (ns).
- `snapshot(ctx)` helper → `mid_px`, `spread_ticks`, `bid_px/ask_px`,
  `bid_qty/ask_qty`, `position_lots`, `cash_lots`.

**Units will bite you.** Prices are integer ticks (`price_scale = 100`,
so $100.00 = 10000 ticks). Quantities are integer lots
(`qty_scale = 1_000_000`). The A-S model takes inventory in whole lots —
convert, or one fill throws your quotes 400k ticks off-touch.

**Gate your requotes.** `on_book_update` fires on nearly every event-loop
tick. Cancel/re-place only when your desired quotes actually change, or
you will hammer the order-rate limiter (the reference A-S bot learned
this the hard way).

**Risk-gate everything.** Cap order qty, cap position, clamp offsets.
The reference `DebateTrader` fails safe: any error → hold, never trade.

## Backtest it

```bash
# your bot vs the built-ins, same seeds, same market, one leaderboard
kairos arena --bots as,fixed --py-bot mybot.py:MyMaker \
    --seeds 20 --duration 30 --regime calm
```

Regimes: `calm`, `volatile`, `informed_heavy`. Thinking budget:
C++ callbacks get 5ms wall-clock; Python bots get `--budget-s`
(default 180s) per callback — the market runs in logical time, so
thinking longer buys no market advantage; the budget only guards hangs.

## Don't fool yourself: the realism gap

Naive backtests assume fills print at your price with zero latency.
Kairos models queue position and latency — and the difference is brutal:

```bash
kairos realism-gap --chart gap.svg
# -> naive fill-at-touch overstates market-maker PnL by ~20-30%
#    vs the risk-averse queue model. Same strategy, same data.
```

Always quote the honest (L3/L4) numbers, never the naive (L1) ones.
Synthetic data: the *relative gap* is the finding; absolute PnL is not
tradable.

## Compete

```bash
kairos season list                 # published competitions
kairos season run season-01        # run it, record it
kairos leaderboard --season season-01
```

Seasons are versioned configs in `data/seasons/`; official results are
committed to `data/leaderboard/ledger.jsonl` — re-run a season to
verify any score. Score = mean PnL − inventory penalty + Sharpe-like term.

## LLM-powered bots

`kairos.agents` ships an OpenAI-compatible client (stdlib only, no new
deps — set `KAIROS_LLM_API_BASE` / `KAIROS_LLM_API_KEY` /
`KAIROS_LLM_MODEL`), a deterministic `MockLLM` for offline runs, and
`DebateTrader`: analyst → bull/bear debate → trader → rule-based risk
gate. See `examples/debate_demo.py`:

```bash
kairos arena --bots none --py-bot examples/debate_demo.py:make_debate \
    --seeds 3 --duration 60
```

Without `KAIROS_LLM_API_KEY` it runs on the mock — good for testing the
plumbing, not for judging the strategy.

## Repo layout (from source)

- `include/kairos/` — engine headers (hot path: no allocs, no locks,
  integer ticks/lots, deterministic)
- `python/kairos/` — pip package: `Simulator`, `agents/`, `seasons.py`,
  `realism.py`, `synth.py`, `leaderboard.py`, `cli.py`
- `strategies/arena_bots.hpp` — built-in C++ bots to crib from
- `docs/adr/` — architecture decisions; read before changing design
- `docs/research/realism-gap.md` — the fill-honesty study
