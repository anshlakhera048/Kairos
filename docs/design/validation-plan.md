# Validation Plan: Calibrating the Fill Model Against Real Fills

Date: 2026-10-03
Status: Design only. NO code in this phase places real orders.

## Goal

The queue models (docs/design/fill-model.md) predict *whether* and *when*
a resting limit order fills. These predictions must be calibrated against
reality before the simulator's output is trusted. This plan describes the
experiment, the data to log, the comparison methodology, and the
statistics to report.

## What is being validated

1. **Fill probability**: given my order's price, size, and the book state
   at placement, the model predicts P(fill within T). The experiment
   measures the empirical fill rate.
2. **Fill time distribution**: the model predicts *when* fills happen
   (via the replay + queue model). The experiment compares predicted vs
   actual time-to-fill distributions.
3. **The p_ahead parameter** (ProbabilisticQueue): the experiment's
   primary calibration target.

The RiskAverseQueue is not calibrated (it is a deliberate bound); it is
validated only in the sense that real fill rates should be >= its
predictions.

## Experimental stages

### Stage 0: Paper validation (no orders, no risk)

Replay a recorded capture through the simulator with a scripted strategy
(place post-only bids at the touch, cancel after N seconds). Record the
*simulated* fill times and probabilities. This produces the model's
predictions in a controlled setting and shakes out logging bugs. Zero
cost, zero risk.

### Stage 1: Venue testnet

Place real orders on the venue's testnet/sandbox (if available) using the
same strategy script. Testnets have fake liquidity and often unrealistic
matching, so this stage validates *plumbing* (order entry, fill callbacks,
logging, clock sync) — not the model.

Note: Coinbase's public sandbox was deprecated; check current availability.
Alternatives: a venue with an active testnet, or skip to Stage 2 with
minimal size.

### Stage 2: Tiny real post-only limit orders

Only after Stage 1 plumbing is solid. Place small post-only limit orders
(minimum size, e.g. $1–5 notional) on the real book:

- **Side**: both bids and asks, at the touch (best bid/ask).
- **Size**: the venue minimum. Small enough that assumption 2
  (no market impact) holds.
- **Lifetime**: cancel after T seconds if unfilled (T = 30–60s).
- **Volume**: hundreds of orders per regime (see below), spread across
  days. This is the expensive stage (fees, even with rebates, and time).

**Regimes** (the md requires parameterized regimes): run the experiment
separately in calm, volatile, and high-spread conditions (classified by
realized volatility and spread from the recorder). The model's accuracy
is expected to differ by regime; report per-regime.

## What to log (per order)

For every order, log a single JSON record:

```
{
  "order_id": "venue-assigned id",
  "symbol": "BTC-USD",
  "side": "bid",
  "price_ticks": 99900,
  "qty_lots": 100000000,
  "post_only": true,
  "t_place_local_ns": ...,     // my clock at send
  "t_place_exchange_ns": ...,  // venue's ack timestamp
  "t_cancel_local_ns": ...,    // if cancelled
  "t_fill_ns": [...],          // venue fill timestamps (partial fills)
  "fill_qty_lots": ...,
  "book_at_place": {           // L2 snapshot at placement
    "bid_qty_at_price": ..., "ask_qty_at_price": ...,
    "spread_ticks": ..., "mid_ticks": ...
  },
  "predicted_p_fill": 0.42,    // model prediction BEFORE the outcome
  "predicted_t_fill_ms": ...,  // model's expected time-to-fill
  "model": "probabilistic",
  "p_ahead": 0.5,
  "seed": 12345,
  "regime": "calm"
}
```

Critical: the prediction must be logged *before* the outcome is known.
Log the simulator's prediction at placement time, then the realized
outcome. Comparing them afterwards is the entire experiment.

Also log the raw market data (the recorder already does) so the
simulation can be re-run deterministically for any order.

## Comparison methodology

For each order, re-run the simulator on the recorded data with the same
model parameters and seed, and compare:

1. **Fill indicator**: predicted P(fill) vs actual {0, 1}.
2. **Time to fill**: predicted distribution (over seeds) vs actual fill
   time (for filled orders).
3. **Queue position proxy**: the model's `qty_ahead` over time vs the
   realized "queue depletion rate" (inferred from trades at my price).

## Statistics to report

- **Calibration curve**: bin orders by predicted P(fill) decile; plot
  mean predicted vs empirical fill rate per bin. A calibrated model lies
  on the diagonal.
- **Brier score**: mean squared error of predicted probabilities.
  Report per-regime and overall.
- **Fill-probability error**: |empirical - predicted| per decile, with
  95% Wilson confidence intervals on the empirical rates.
- **Time-to-fill**: compare predicted vs actual distributions with a
  Q-Q plot; report median absolute error and the Kolmogorov-Smirnov
  statistic.
- **Ablation**: RiskAverseQueue vs ProbabilisticQueue (p=0.5) vs
  calibrated p — Brier scores side by side, to show the calibration
  actually helped.

## Calibration procedure

1. Split orders 50/50 into calibration and holdout sets (stratified by
   regime).
2. On the calibration set, grid-search p_ahead in [0, 1] (step 0.05) to
   minimize Brier score.
3. Report the calibrated p_ahead and its Brier score on the HOLDOUT set.
   (Reporting calibration-set performance is overfitting.)
4. If the calibration curve is systematically off-diagonal even after
   tuning p_ahead, the model family is misspecified — document it and
   consider a state-dependent p_ahead (documented as future work, not
   implemented here).

## Sample size guidance

To estimate a fill rate within ±5% at 95% confidence needs ~400 orders
per regime (Wilson interval). Three regimes → ~1200 orders. At minimum
size and post-only (rebate), the expected cost is small but nonzero;
budget it explicitly before Stage 2.

## Considerations you must check yourself

**Legal / tax:**
- Trading profits (even tiny) may be taxable in your jurisdiction. Keep
  records of every fill; export the venue's tax reports.
- If you are employed, check your employer's personal-trading policy.

**Exchange ToS:**
- Read the venue's API terms: rate limits, prohibited behaviors
  (quote stuffing, layering — this experiment must NOT resemble
  manipulative patterns; it places small bona fide orders and cancels
  them, which is normal market-making behavior, but verify).
- Testnet vs production API keys have different permissions; confirm.

**Key permissions:**
- Use **trade-only** API keys (no withdrawal permission) for any stage
  that touches production. IP-whitelist the keys if the venue supports
  it. Never commit keys to the repo (the recorder and simulator never
  take keys as arguments — a future order tool will read them from the
  environment).

**Operational:**
- Hard cap: max order size, max position (zero — cancel-all flattens),
  max orders per minute, kill switch (a file whose presence halts the
  tool). These are required before Stage 2, not optional.
- Start Stage 2 during calm, liquid hours; avoid news events and
  maintenance windows.

## Gate

**Do NOT proceed to any real-order tooling until you confirm in writing
that you want to.** When you do, the build order is: (1) the heavily
rate-limited order tool with the caps above, (2) Stage 1 dry runs,
(3) Stage 2. The safety rules in the Safety section apply throughout.
