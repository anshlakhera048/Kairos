# The Realism Gap: How Much Does Fill Realism Cost a Market Maker?

Date: 2026-10-03
Status: Research note (Phase 3D)

## Question

A naive backtest assumes my limit orders fill immediately when a trade
prints at my price ("fill at touch") with zero latency. A realistic
simulation models feed/order latency and queue position. **How much PnL
does the naive assumption overstate?**

## Method

The same Avellaneda-Stoikov market-making strategy runs on the same
30-minute synthetic L2 capture under four realism levels:

- **L1 (naive)**: fill at touch, zero latency. My orders are assumed
  first in queue; any trade at my price fills me immediately.
- **L2 (+ latency)**: 5ms feed latency, 2ms order latency. Queue still
  naive.
- **L3 (+ risk-averse queue)**: latency plus back-of-queue assumption —
  only trades advance my queue position.
- **L4 (+ probabilistic queue)**: latency plus Binomial(0.5) cancel
  attribution.

The strategy quotes both sides at the touch (clamped from A-S optimal),
500ms requote timer, post-only, 10bp maker rebate / 30bp taker fee.
All runs are deterministic (fixed seeds).

## Data

Synthetic L2 feed (`tools/recorder/synth_market.py`): random-walk mid
(σ = 0.02%/√s), 10 levels/side, Poisson trades (3/s) with
exponential distance from touch (κ = 2.0). 19,757 events, 5,428 trades.
Seed 7.

*Limitation: synthetic, not real market data. The random walk has no
informed flow, so adverse selection is absent by construction. The
absolute PnL numbers are not tradable; the *relative* gap between
realism levels is the finding.*

## Results

| Run | PnL ($) | 95% CI | Fills | Fill rate |
|---|---|---|---|---|
| L1 naive | 154.10 | (151.0, 162.8) | 1,783 | 4.6% |
| L2 + latency | 153.99 | (150.2, 164.2) | 1,769 | 4.6% |
| L3 + risk-averse | 124.56 | (121.9, 133.5) | 1,423 | 3.8% |
| L4 + probabilistic | 128.19 | (125.1, 137.3) | 1,454 | 3.8% |

(CIs via block bootstrap on per-minute PnL, 1,000 resamples.)

**The realism gap: moving from naive (L1) to a realistic queue model
(L3/L4) reduces PnL by ~19% and fills by ~20%.** The 95% CIs do not
overlap — the gap is statistically significant.

Latency alone (L1 → L2) barely matters here: with a naive queue, fills
are immediate regardless of delay. The queue model is the dominant
realism factor, not latency — for *this* strategy on *this* data. (A
taker-heavy strategy would show a larger latency effect.)

Markouts (post-fill mid-price move) are small and positive (~$0.007 at
1s and 60s) across all levels: the synthetic market has no informed
flow, so fills are not adversely selected. In real data, expect
negative markouts, larger for naive fills.

## Limitations

1. **Synthetic data.** No informed traders, no volatility clustering,
   no feed gaps. The gap direction (naive overstates) should hold on
   real data; the magnitude will differ.
2. **Single seed, single regime.** One 30-minute calm-market sample.
   Volatile regimes would widen the gap (queues churn faster).
3. **Uncalibrated probabilistic model.** L4 uses p_ahead = 0.5 (a guess).
   The validation plan (docs/design/validation-plan.md) describes how
   to calibrate it against real fills; until then, L3 (risk-averse) is
   the conservative choice.
4. **Strategy quotes at touch.** The A-S optimal quotes are clamped to
   the touch for backtest alignment (synthetic trades only print where
   the historical book had liquidity). A live strategy would improve the
   touch.

## What would falsify the conclusion

- If L3/L4 PnL ≥ L1 PnL on real market data (i.e., realistic queue
  modeling does *not* reduce estimated PnL), the "naive overstates"
  claim is wrong — possibly because real queues are shorter than the
  model assumes, or because the strategy's edge dominates fill effects.
- If the gap vanishes after calibrating p_ahead on real fills, then the
  gap was a calibration artifact, not a structural bias.

## Reproduction

One command regenerates the data, figures, and this table:

```
scripts/realism_gap.sh
```

It runs: synth_market.py → kairos_realism_gap → analyze.py. All seeds
fixed. Figures in `docs/research/figures/`.
