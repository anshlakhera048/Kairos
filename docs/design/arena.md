# Arena Design (Phase 4)

## Goal

A local-first tournament harness where bots compete against simulated
order flow on the same matching engine, scored fairly and reproducibly.

## Architecture

**One bot per Arena run.** Each bot faces identical market conditions
(same seeds, same flow generators, same latency, same fees). The
"tournament" is N bots × M seeds = N×M independent runs, compared via
the leaderboard. Bots do not interact with each other — this is fairer
and more reproducible than a shared free-for-all.

**Live engine, not pre-generated flow.** Flow generators (noise,
informed, background makers) and the participant bot all submit orders
to one shared `Engine` (the Phase 1 matching core), stepped forward in
logical time. The bot's orders rest in the book and get hit by flow —
it has market impact. This is what distinguishes the arena from a
backtest.

## Event Loop

Single-threaded, deterministic. Logical time advances to the next event:
- Flow generator actions (each has a `next_time()`).
- Bot pending orders (effect time = submit time + order latency).
- Delayed book view deliveries (effect time + feed latency).
- Bot timer (periodic).

At each time step:
1. Generators act on the engine.
2. Matured bot orders are submitted to the engine.
3. Bot fills are detected by polling `OrderBook::get_order()` (new
   read-only API; the engine's events are taker-centric and don't
   identify makers).
4. Delayed view updates are delivered (snapshot via
   `OrderBook::snapshot_levels()`, new API).
5. Bot timer fires.

## Flow Generators (4A)

Deterministic from seed (splitmix64 per generator):
- **NoiseTrader**: Poisson arrivals, 40% market / 60% limit, lognormal
  sizes, exponential price placement. The "dumb" flow that pays spread.
- **InformedTrader**: observes latent fair value (random walk + jumps),
  trades via market orders when |fair - mid| > threshold. Creates
  adverse selection — the Glosten-Milgrom mechanism.
- **FixedSpreadMaker**: quotes at fixed spread, refreshes on timer.
  Baseline liquidity.
- **MomentumTrader**: EWMA trend follower. Creates flow autocorrelation.
- **Regimes**: calm / volatile / informed_heavy presets.

## Fairness (4B)

- Same logical latency for all bots (5ms feed, 2ms order).
- Same delayed book view (10 levels/side via snapshot).
- Identical fees (-10bp maker, +30bp taker).
- Same seeds → identical flow.

## Scoring (4B)

`total = mean(PnL) - λ_inv × inventory_penalty + λ_risk × risk_adjusted`

- **PnL**: mean realized PnL across seeds (dollars).
- **Inventory penalty**: mean |position| × spread_cost. Charges for
  warehousing risk.
- **Risk-adjusted**: Sharpe-like (mean / std of per-seed PnL). Rewards
  consistency.

Why not raw PnL: a bot can take huge directional bets and get lucky on
one seed. The penalty and risk terms punish this.

## Anti-Cheat (4B)

Enforced by the harness:
- **No future info**: bot sees only the delayed view (structural, like
  Phase 3).
- **Time budget**: 5ms per callback; exceed → disqualified.
- **Position limit**: 10M lots; breaching orders rejected (counted).
- **Rate limit**: 100 orders/sec; exceed → rejected.

The `CheaterBot` (tries to buy 1B lots) gets all orders rejected —
verified by test.

## Bot API

Reuses the Phase 3 Strategy concept (seven callbacks). The Arena's
`Context` provides `now()`, `book()`, `account()`, `send_limit()`,
`cancel()`, `modify()` — same interface as the Simulator, so bots work
in both.

## Limitations

- Fill detection via polling is O(bots × orders) per time step. Fine
  for a tournament, not for HFT.
- The view snapshot is top-10 levels only; deeper book not visible.
- No partial-fill price improvement tracking (simplified).
- Python bots not yet integrated into the tournament driver (the
  bindings exist; wiring them into `run_tournament` is future work).
