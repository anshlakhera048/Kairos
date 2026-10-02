# ADR 0003: Caller-provided event buffer with atomicity guarantee

Date: 2026-10-03
Status: Accepted

## Context

The engine must emit ack/reject/fill/cancel/modify events with no allocation
on the hot path. The number of fills per operation is data-dependent (a sweep
can consume thousands of resting orders), so a fixed-size event struct alone
does not bound the output.

## Decision

The caller provides `Event* out` + capacity; the engine returns the event
count. Before mutating any state, the engine checks a cheap upper bound:

- `add`: at most `live_count + 1` events (one per resting order touched,
  plus the Ack)
- `modify`: at most `live_count + 2` (Cancel + fills + Ack)
- `cancel`: exactly 1

If the buffer is too small, `EventBufferFull` is returned and **nothing is
mutated**. Operations are therefore atomic with respect to the event buffer:
the caller never has to reason about partial application.

## Alternatives considered

- `std::function` callback per event: may allocate, indirect call, and gives
  the engine no backpressure signal.
- Virtual `EventSink`: banned virtuals on the hot path.
- Returning `std::vector<Event>`: allocation per operation.
- Single-pass emit with mid-stream overflow error: leaves the book mutated
  but the event stream truncated — the worst outcome for a simulator, where
  the event log *is* the record of what happened.

## Consequences

- Callers size the buffer once (e.g. `live_count + 2`) and never think about
  it again. Tests use a generously sized vector.
- The bound is loose (most ops emit 1–3 events) but O(1) to check. A tighter
  bound would require a read-only pre-scan duplicating the match walk; the
  FOK quantity check already does such a scan where exactness is required
  (all-or-nothing semantics), and that scan is the exception, not the rule.
- `Event` stays trivially copyable and fixed-size, so buffers can be stack
  arrays, ring buffers, or mmap'd logs.
