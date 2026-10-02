# ADR 0002: Tick-band level array for price levels

Date: 2026-10-03
Status: Accepted

## Context

The book needs O(1) lookup of the price level for an incoming order's price,
with strong cache locality on the hot path. Price is an `int64_t` tick; the
level container is touched on every add, cancel, and match step.

## Decision

Fixed array indexed by tick offset: `levels[price.ticks - band_lo]`, covering
a configurable band (default ±1024 ticks around a base price). Prices outside
the band are rejected with `OutOfBandPrice`.

## Alternatives considered

- `std::map<Price, Level>`: banned by the project's own rules (node
  allocation, pointer chasing, O(log n)).
- Sorted vector + binary search: O(log n) probe and O(n) memmove when a new
  level appears between existing ones.
- Hash map price → level: hashing cost on every touch and worse locality
  than a single subtraction.

## Consequences

- Level lookup is one subtraction + bounds check; levels are contiguous
  (~96KB for the default band), so hot levels stay in L1/L2.
- The engine cannot represent prices outside the band. This is a documented
  limitation, not a silent behavior: out-of-band orders are rejected, never
  clamped or ignored. Multi-instrument use runs one engine per instrument
  with per-instrument bands.
- Rejected alternative "dynamic rebasing" because it introduces a latency
  spike at the worst moment and complicates the determinism story.
