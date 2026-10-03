# Python Bindings: Performance Notes (Phase 3B)

The Python path exists for research velocity, not production speed.
Here is the measured cost, on the 19,757-event synthetic capture
(AMD EPYC shared VM, GCC -O2 for C++):

| Path | Events/sec | Relative |
|---|---|---|
| C++ A-S strategy | 132,009 | 1.0x |
| Python do-nothing strategy | 48,992 | 2.7x slower |
| Python A-S strategy | 11,390 | 11.6x slower |

## Where the time goes

**Binding overhead (~2.7x):** Every callback pays for GIL acquisition,
constructing the `Context` value object, building Python dicts for
trade/fill/ack info, and Python method dispatch. The C++ event loop
(book updates, queue tracking, latency sampling) runs with the GIL
released, so this is purely the boundary cost — roughly 20µs per
market event in the do-nothing case.

**Strategy logic (~4.3x on top):** The Python A-S does the same math as
C++ but without inlining, without compile-time dispatch, and with
Python float overhead per quote. This dominates once the strategy does
real work.

## What this means

- **Prototyping:** The Python path is fine. 11k events/sec handles a
  30-minute capture in under 2 seconds. Iterate on logic in Python,
  port to C++ when the logic is settled.
- **Parameter sweeps:** Python is acceptable for dozens of runs; for
  hundreds, use the C++ path.
- **Production or the Phase 4 arena:** Use C++. The 11.6x gap is
  structural (GIL + interpreter), not a tuning issue.
- **Correctness:** The Python A-S produces bit-identical quotes to the
  C++ implementation for the same inputs (verified on test vectors).
  The strategies diverge only in speed, not logic.

## Design choices

- The C++ `Simulator` keeps compile-time dispatch (templates/concepts).
  `PyStrategy` is a C++ adapter satisfying the `Strategy` concept; it
  forwards to Python callables. The fast path is untouched — no virtuals
  were added to the hot loop for Python's benefit.
- Results come back as dicts of numpy arrays (`fills`, `equity`) plus a
  `stats` dict. Convert to pandas with `pd.DataFrame(result['fills'])`.
- Callbacks receive a `Context` value object (not a reference wrapper):
  `now_ns`, `best_bid()`/`best_ask()`, `position_lots`, `send_limit()`,
  `cancel()`, `modify()`. Trade/fill/ack details arrive as dicts.
- If the Python strategy raises, the exception propagates out of
  `run()` — the C++ side does not swallow it.
