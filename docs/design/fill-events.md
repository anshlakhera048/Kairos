# Fill Events: Taker-Perspective vs Maker-Side

Date: 2026-10-03
Status: Recommendation (decision deferred to Phase 4)

## Context

Phase 1's event stream emits **taker-perspective fills only**: when an
aggressive order consumes resting liquidity, one `Fill` event is emitted per
resting order consumed, carrying the *taker's* order id, the fill price, and
the fill quantity. The maker (owner of the resting order) gets no event.

This is sufficient for the Phase 2 simulator (single strategy: the strategy
is always the taker of its own fills, or it can infer its resting fills from
its own order state). It is **not** sufficient for the Phase 4 arena, where
multiple bots interact: when Bot B's aggressive order consumes Bot A's resting
order, Bot A must learn about its fill from the event stream.

## Design A: Taker-perspective only (current)

```
Fill { order_id: taker_id, price, qty }
```

- One event per resting order consumed.
- Simple, minimal event volume.
- Maker infers fills by tracking its own resting orders (works single-player,
  breaks multi-player).

## Design B: Dual events (taker Fill + maker Fill)

```
Fill { order_id: taker_id, price, qty, side: Taker }
Fill { order_id: maker_id, price, qty, side: Maker }
```

- Two events per fill; consumer filters by its own id and the side flag.
- Doubles fill-event volume (the dominant event type in aggressive flows).
- Requires a side/role flag to disambiguate, or consumers double-count.

## Design C: Single fill event with both ids (recommended)

```
Fill { order_id: taker_id, counterparty_id: maker_id, price, qty }
```

- One event per fill (no volume increase over Design A).
- Taker identifies its fills by `order_id == my_id`; maker by
  `counterparty_id == my_id`. No ambiguity, no double-counting.
- The engine already knows the maker id (it's the resting order's id);
  the field is free to populate.

## Recommendation

**Design C, implemented when Phase 4 begins.** Rationale:

1. Phase 2 (single-strategy simulator) does not need maker-side fills.
   The strategy knows its own resting orders; taker-perspective fills plus
   its own order state are complete. YAGNI applies.
2. Adding `counterparty_id` to the `Fill` event is a minimal, backward-
   compatible struct change (one `uint64_t`). Doing it in Phase 4 keeps
   Phase 2's event stream and differential tests stable.
3. Design B (dual events) is strictly worse: 2× event volume for zero
   additional information over Design C.

## Consequences

- Phase 2 proceeds with taker-only fills. No engine change.
- Phase 4 will extend `Event` with `counterparty_id` (populated on Fill
  events, zero otherwise), update the reference implementation and
  differential test, and document the filtering contract for bots.
- If a Phase 2 use case emerges that needs maker-side fills (e.g.,
  simulating a market-maker's resting fills against historical taker flow),
  revisit: the field can be added without breaking the stream layout if
  appended to the struct.
