#!/usr/bin/env python3
"""Smoke test for the Kairos Python bindings (Phase 3B).

Run from the repo root after building:
    cmake --build --preset debug --target kairos_py
    PYTHONPATH=python python3 python/test_bindings.py /tmp/synth_exp.kai
"""

import sys

sys.path.insert(0, "python")
import kairos
from kairos.as_strategy import ASModel, AvellanedaStoikov


def test_quote_parity():
    """Python A-S quotes must match the C++ implementation exactly."""
    m = ASModel(gamma=1500.0, sigma=0.0002, kappa=2.0, horizon_s=3600.0)
    # (mid, inventory) -> expected (bid, ask) from C++ avellaneda_stoikov_quotes.
    cases = [
        ((10000, 0), (9999, 10001)),
        ((10000, 1000000), (-422000, -205999)),
        ((10000, -5000000), (1089999, 2170000)),
    ]
    for (mid, inv), expected in cases:
        got = m.quotes(mid, inv)
        assert got == expected, f"mid={mid} inv={inv}: got {got}, want {expected}"
    print("quote parity: OK")


def test_simulator_runs(capture_path):
    """The simulator runs a Python strategy and returns numpy arrays."""

    class CountCallbacks:
        def __init__(self):
            self.n_book = 0
            self.n_timer = 0

        def on_book_update(self, ctx):
            self.n_book += 1
            # Exercise the Context API.
            assert ctx.now_ns >= 0
            bb = ctx.best_bid()
            ba = ctx.best_ask()
            if bb is not None:
                assert bb[0] > 0 and bb[1] >= 0
            if ba is not None:
                assert ba[0] > 0 and ba[1] >= 0

        def on_trade(self, ctx, trade):
            assert "price_ticks" in trade

        def on_own_fill(self, ctx, fill):
            pass

        def on_ack(self, ctx, ack):
            pass

        def on_reject(self, ctx, order_id):
            pass

        def on_cancel(self, ctx, order_id):
            pass

        def on_timer(self, ctx):
            self.n_timer += 1

    sim = kairos.Simulator(queue_model="naive")
    strat = CountCallbacks()
    res = sim.run(capture_path, strat, timer_interval_ns=500_000_000)

    assert res["stats"]["market_events"] > 0, "no events processed"
    assert strat.n_book > 0, "on_book_update never called"
    assert strat.n_timer > 0, "on_timer never called"
    # Numpy arrays are present and well-formed.
    assert res["fills"]["t_ns"].shape[0] == 0  # do-nothing: no fills
    # Equity is sampled on timer when the book has two sides; may be fewer.
    assert res["equity"]["t_ns"].shape[0] <= strat.n_timer
    assert res["equity"]["t_ns"].shape[0] > 0
    print(f"simulator run: OK ({res['stats']['market_events']} events, "
          f"{strat.n_book} book callbacks)")


def test_as_strategy_runs(capture_path):
    """The Python A-S strategy gets fills."""
    sim = kairos.Simulator(
        feed_latency_ns=5_000_000,
        order_latency_ns=2_000_000,
        queue_model="risk_averse",
        maker_fee_bp=-10,
        taker_fee_bp=30,
    )
    res = sim.run(capture_path, AvellanedaStoikov(),
                  timer_interval_ns=500_000_000)
    n_fills = res["stats"]["fills"]
    assert n_fills > 0, "A-S strategy got zero fills"
    print(f"A-S strategy: OK ({n_fills} fills)")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(f"usage: {sys.argv[0]} <capture.kai>")
        sys.exit(1)
    test_quote_parity()
    test_simulator_runs(sys.argv[1])
    test_as_strategy_runs(sys.argv[1])
    print("all Python binding tests passed")
