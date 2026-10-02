# Order Book Design

The matching engine at the heart of Kairos: a limit order book with
price-time priority, deterministic behavior, and a hot path that never
allocates, locks, throws, or dispatches virtually.

## Requirements (from the Master Brief)

- Order types: limit, market, IOC, FOK, post-only.
- Price-time priority; modify semantics: reduce size keeps priority, price
  change or size increase = cancel/replace (loses priority).
- Self-trade handling as a documented configurable policy.
- Allocation-free event output; error handling without exceptions.
- Deterministic: identical input + seed => bit-identical output.
- Differential-tested against a naive reference (10M+ ops, 5+ seeds).

## Architecture

```
                          ┌─────────────┐
                          │  OrderBook  │
                          └──────┬──────┘
              ┌──────────────────┼──────────────────┐
              │                  │                  │
     ┌────────▼────────┐ ┌──────▼──────┐  ┌────────▼────────┐
     │ Level array     │ │ Order pool  │  │ Id hash map     │
     │ [tick offset]   │ │ + free list │  │ (open addressing│
     │ O(1) lookup     │ │ intrusive   │  │  linear probing)│
     └────────┬────────┘ │ FIFO links  │  └─────────────────┘
              │          └─────────────┘
     ┌────────▼────────┐
     │ Hierarchical    │
     │ bitmap          │
     │ (best bid/ask)  │
     └─────────────────┘
```

### Price levels: tick-offset array

`levels[price.ticks - band_lo]` — a fixed array covering a configurable band
(default ±1024 ticks around a base price). Lookup is one subtraction and one
bounds check: O(1), no hashing, no pointer chasing. Levels are contiguous in
memory (48 bytes each, 2048 levels ≈ 96KB), so the hot levels stay in L1/L2.

Prices outside the band are rejected with `OutOfBandPrice`. This is a
deliberate choice over dynamic rebasing: rebasing is a latency spike at the
worst moment and complicates the determinism story. The band is configurable
per instrument; a multi-instrument setup runs one engine per instrument.

Alternatives considered: `std::map` price→level (banned: node allocation,
pointer chasing, O(log n)); sorted vector + binary search (O(log n) probe
plus O(n) memmove on level insert); hash map price→level (hashing cost on
every touch, worse locality than a subtracted index).

### FIFO queues: intrusive doubly-linked list

Each price level holds a bid queue and an ask queue. Orders link via `prev` /
`next` **pool indices** (not pointers): enqueue at tail, dequeue from head,
cancel from the middle — all O(1), zero allocation, links live inside the
already-allocated `Order`. Indices never invalidate and serialize trivially.

Cost: 8 bytes per order for the two links, inside the 64-byte `Order` budget.

Alternatives: `std::list`/`std::deque` per level (per-node allocation,
cache-hostile); flat array + memmove (O(n) cancel); per-level ring buffer
(fixed capacity wastes memory or overflows under skew).

### Order storage: pool + free list, generational handles

A fixed-capacity pool (default 2^20 orders ≈ 56MB) with a LIFO free-list
stack. Allocation is a pop, freeing is a push — both O(1), and LIFO reuse
keeps hot slots in cache. Each slot carries a generation counter, bumped on
reuse, defeating ABA bugs if a slot is recycled while a stale handle exists.

`Order` layout (56 bytes, ≤ one cache line), fields ordered by access
frequency: id, price, remaining qty, time-priority seq — the fields the match
loop touches every time — then links, generation, owner, side, type.

Alternatives: `new`/`delete` per order (banned: allocator latency,
fragmentation); vector + swap-remove (breaks stable references).

### OrderId → order: flat open-addressing hash map

Cancel and modify need O(1) id lookup. A linear-probing table with
power-of-two capacity (2× pool size, so load never exceeds 0.5), splitmix64
hash, and backward-shift deletion (no tombstone accumulation). Fixed capacity,
zero allocation after construction; the common hit is one or two cache lines.

When the pool is exhausted, new orders are rejected with `NoCapacity` —
deterministic backpressure, never silent degradation.

Alternatives: `std::unordered_map` (banned); direct array indexed by id
(64-bit id space is impossibly sparse); tree (O(log n) + node allocation).

### Best bid/ask: hierarchical bitmap

One bit per level per side: 2048 levels → 32×uint64 at L0, 1×uint64 at L1
(≈ 528 bytes total). Best bid = highest set bit, best ask = lowest set bit,
via `std::countl_zero` / `std::countr_zero` — effectively O(1), a handful of
cycles. Bits flip only on level empty↔non-empty transitions.

Alternatives: cached best bid/ask variables (O(1) on add, but cancelling the
best level degrades to an O(band) scan at exactly the wrong moment);
`std::map` (banned).

### Event output: caller-provided buffer

`add` / `cancel` / `modify` write into `Event* out` with an explicit capacity
and return the count. The engine never allocates; the caller owns the memory
(stack array, ring buffer, mmap'd log). `Event` is trivially copyable and
fixed-size (48 bytes), so it can be memcpy'd and replayed bit-identically.

**Atomicity:** an `add` emits at most `live_count + 1` events (one per resting
order touched, plus the Ack) and a `modify` at most `live_count + 2`. The
capacity check happens *before* any state is touched: if the buffer is too
small, `EventBufferFull` is returned and nothing is mutated. Operations are
atomic with respect to the event buffer — there is no partial-application
state for the caller to reason about.

Alternatives: `std::function` callback (may allocate, indirect call);
virtual sink (banned); returning `std::vector<Event>` (allocation).

### Event stream contract

- Every mutating op emits exactly one *disposition* event (Ack, Cancel,
  Modify, Reject) plus zero or more Fill events.
- Fills are **taker-perspective**: one Fill per resting order consumed,
  carrying the taker's order id, the fill price, and the fill qty.
- `Ack.qty` = amount now **resting** (0 when fully filled or discarded —
  IOC/Market remainder is discarded, not rested).
- `seq` is a dense deterministic event sequence (1, 2, 3, …); `ts` is the
  explicit timestamp passed with the operation.
- `Cancel` carries the cancelled qty; `Modify` (in-place reduce only)
  carries the new state; `Reject` carries the unfilled remainder.

### Modify semantics

- Same price, non-increased quantity → in-place reduction. Keeps queue
  priority, emits one `Modify` event.
- Otherwise (price change or quantity increase) → cancel/replace: the order
  loses time priority. Emits `Cancel` (old state) followed by the normal add
  sequence (fills + Ack) under the same id. The event stream says exactly
  what happened — no synthetic "replaced" event to interpret.

### Self-trade policy

`SelfTradePolicy::{Allow, CancelResting, CancelIncoming}`, configured per
engine. Applies only between two **nonzero, equal** owners (`owner == 0`
means anonymous). Default `Allow`: single-trader simulation needs no
configuration. Multi-participant use (the Phase 4 arena) assigns distinct
nonzero owners per participant:

- `CancelResting`: the resting order is removed (a `Cancel` event is
  emitted for it) and the incoming order continues past it.
- `CancelIncoming`: matching stops immediately; fills already made stand
  and the remainder is rejected (`SelfTradeReject`).

### Error handling

`Error` enum returned by value; the hot path is `noexcept` throughout. No
exceptions, no errno globals, no boolean traps. Validation order is fixed
and documented (args → duplicate id → capacity → band → event buffer →
post-only → FOK), so error precedence is deterministic and mirrored by the
reference implementation.

## Matching algorithm

```
add(id, side, type, price, qty):
  validate (fixed precedence)
  post-only: reject if it would cross the touch
  FOK: read-only pre-scan of fillable qty; reject if insufficient
  loop while qty remains:
    best opposite level (bitmap); stop if none or not crossing
    (market orders ignore the price bound)
    walk the level's FIFO queue:
      self-trade check (policy)
      fill = min(remaining, resting.qty); emit Fill
      fully consumed -> unlink, erase from id map, free slot
  remainder:
    Limit/PostOnly -> rest in book, Ack{resting qty}
    IOC/Market    -> discard, Ack{0}
```

The FOK pre-scan is read-only and exact (it respects the self-trade policy),
so FOK is genuinely all-or-nothing with no rollback path.

## Complexity summary

| Operation | Time | Notes |
|---|---|---|
| add (resting) | O(1) | hash insert + queue push + bitmap set |
| add (aggressive) | O(fills + levels) | one pass over consumed orders |
| cancel | O(1) avg | hash lookup + unlink |
| modify (reduce) | O(1) avg | hash lookup + qty update |
| modify (replace) | O(fills) | cancel/replace path |
| best bid/ask | O(1) | two bit-scans |
| FOK check | O(touched orders) | read-only pre-scan |

Memory: pool (56B × max_orders) + id map (16B × 2 × max_orders) + levels
(48B × 2 × half_band) + bitmaps (≈ 0.5KB). Default config ≈ 88MB.

## Testing

- **Unit** (`tests/unit/test_orderbook.cpp`): every order type, price-time
  priority, modify semantics, self-trade policies, error paths, event-buffer
  atomicity. Invariants checked after every op.
- **Differential** (`tests/differential/`): naive `std::map`+`std::list`
  reference; identical random op streams; bit-identical event streams and
  error codes required. 10M ops × 5 seeds, plus adversarial distributions
  (heavy cancels, deep sweeps, tight spreads).
- **Randomized/propert
...[truncated 1005 chars]