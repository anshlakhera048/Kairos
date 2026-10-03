# Fill Model: Assumptions and Known Failure Modes

Date: 2026-10-03
Status: Phase 2D

## What the model does

The simulator replays recorded L2 market data and models how *my* limit
orders would have been filled. L2 data publishes aggregate quantity per
price level — never individual orders — so queue position within a level
cannot be observed. It must be **modelled**. This document lists every
assumption and how each one can be wrong.

## Shared assumptions (both queue models)

1. **Price-time priority at the venue.** New quantity joining my level
   goes *behind* me; trades consume from the *front*. True for Coinbase
   (and most central limit order books). Fails on pro-rata venues or
   venues with special order types that jump the queue.

2. **My order does not move the market.** The recorded feed is treated as
   exogenous: my simulated orders neither add to level quantities nor
   trigger trades. Valid for small sizes relative to the level; fails for
   large orders that would have consumed the level or attracted activity.

3. **Initial position: back of queue.** On placement, `qty_ahead =
   level_qty - my_qty`. This is the conservative choice (I just arrived).
   It is wrong if the level was empty (then I am alone at the front —
   the formula gives `qty_ahead = 0`, which is correct) or if the feed's
   level quantity is stale.

4. **Trades at my price consume my level front-to-back.** A recorded trade
   at price P with size V reduces `qty_ahead` by V, then fills me for
   `min(remaining, V - qty_ahead)`. Assumes the trade executed against
   *this* level (true by definition of "trade at price P") and that no
   hidden liquidity (icebergs, dark) absorbed part of it. Hidden liquidity
   makes real fills *worse* than modelled (the trade didn't touch the
   visible queue).

5. **Trade through my price = full fill.** If a trade prints at a price
   better than mine (below for my bid, above for my ask), the level was
   swept and I am filled completely. Fails if the through-trade was a
   crossed/busted trade later cancelled, or if my order would have been
   cancelled before the sweep (cancel latency race — modelled separately).

6. **The feed is authoritative about totals.** If the level quantity drops
   below `qty_ahead + remaining`, the model clamps `qty_ahead` rather than
   contradict the feed. This silently converts unexplained shrinkage into
   "ahead cancelled", which is optimistic.

7. **No partial-visibility effects.** The model sees every diff and trade.
   In reality, websocket feeds can drop messages (the recorder resyncs on
   gaps, but a resync replaces the book — queue state for my order is
   re-derived, losing the true ahead/behind split).

## RiskAverseQueue: additional assumptions

8. **Only trades advance me.** Level reductions not explained by trades are
   ignored for `qty_ahead` (treated as cancels behind me). This is
   deliberately pessimistic: real cancels ahead of me *do* advance my
   position, so this model under-predicts fill rates. Use it for
   worst-case analysis, not calibration.

9. **Back of queue forever.** I never move up except via trades or forced
   clamping. In reality, cancels ahead of me (the common case in a busy
   book) move me up continuously.

Failure mode: on a quiet level with heavy cancel/replace activity and few
trades, the model predicts ~zero fills while a real order would likely
have been filled. The bias is one-sided (pessimistic) by design.

## ProbabilisticQueue: additional assumptions

10. **Binomial attribution of cancels.** A level reduction of D lots is
    split into "ahead" vs "behind" by sampling Binomial(D, p_ahead). This
    assumes each cancelled lot is independently ahead-of-me with
    probability p_ahead — i.e., cancels are uniformly distributed through
    the queue. Real cancel distributions are *not* uniform: high-frequency
    quotes at the front cancel more often; stale orders at the back sit.
    The uniform assumption is a modelling convenience, not an empirical
    fact.

11. **Constant p_ahead.** The implementation uses a single tunable
    probability. In reality, the ahead/behind cancel mix depends on queue
    length, time of day, volatility, and my time in queue. A constant is a
    first-order approximation; the validation plan (docs/design/
    validation-plan.md) describes how to calibrate it.

12. **Deterministic RNG, seed-controlled.** Attribution is reproducible
    given the seed, but a *different* seed gives a different fill path.
    Reported fill probabilities must average over seeds (Monte Carlo) —
    a single seed is not a result.

Failure mode: with p_ahead miscalibrated (e.g., 0.5 on a book where
front-quotes cancel 90% of the time), the model systematically over- or
under-predicts fills. The error compounds over long resting times.

## Account assumptions

13. **Average-cost inventory.** Realized PnL uses average entry price.
    Fine for PnL attribution; does not model tax lots.

14. **Fees are linear in notional.** Maker/taker rates as integer basis
    points, applied per fill. Tiered fee schedules, volume discounts, and
    minimum fees are not modelled.

15. **No funding, no borrow costs.** Short positions accrue no borrow fee;
    perpetual futures funding is not modelled. (Spot simulation only.)

16. **Infinite credit.** The account never rejects an order for
    insufficient margin/cash and never liquidates.

## What is NOT modelled (explicit non-goals for Phase 2)

- Market impact of my own orders (assumption 2).
- Partial fills from my own aggressive orders crossing the spread — the
  Phase 2 simulator only models *resting* limit fills against historical
  flow. (Taker fills are deterministic given the book: if I cross, I get
  the visible quantity. That path is exercised in Phase 3.)
- Post-only reject logic against the *simulated* book (the engine's
  post-only validation is tested in Phase 1 against the matching core).
- Exchange downtime, halts, or feed anomalies beyond gap-resync.
