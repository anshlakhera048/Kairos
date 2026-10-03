"""Kairos Python API (Phase 3B).

The fast path is C++. This package is for research: write a strategy in
Python, run it on captured data, get numpy arrays back.

Quick start:
    import kairos
    from kairos.as_strategy import AvellanedaStoikov

    sim = kairos.Simulator(feed_latency_ns=5_000_000,
                           order_latency_ns=2_000_000,
                           queue_model="risk_averse")
    result = sim.run("/tmp/data.kai", AvellanedaStoikov(), timer_interval_ns=500_000_000)
    fills = result["fills"]  # dict of numpy arrays

See docs/python.md for the overhead discussion (the honest version).
"""

from .kairos_py import Context, SimConfig, run as _run


class Simulator:
    """Event-driven simulator with a Python strategy.

    Args:
        feed_latency_ns: market data delay (exchange -> strategy).
        order_latency_ns: order delay (strategy -> exchange).
        queue_model: 'naive', 'risk_averse', or 'probabilistic'.
        p_ahead: for 'probabilistic', P(a cancelled lot was ahead of me).
        maker_fee_bp: maker fee in basis points (negative = rebate).
        taker_fee_bp: taker fee in basis points.
        seed: deterministic RNG seed.
    """

    def __init__(self, feed_latency_ns=0, order_latency_ns=0,
                 queue_model="risk_averse", p_ahead=0.5,
                 maker_fee_bp=0, taker_fee_bp=0, seed=0x9E3779B97F4A7C15):
        cfg = SimConfig()
        cfg.feed_latency_ns = feed_latency_ns
        cfg.order_latency_ns = order_latency_ns
        cfg.queue_model = queue_model
        cfg.p_ahead = p_ahead
        cfg.maker_fee_bp = maker_fee_bp
        cfg.taker_fee_bp = taker_fee_bp
        cfg.seed = seed
        self._cfg = cfg

    def run(self, capture_path, strategy, timer_interval_ns=0):
        """Run the strategy on a capture file.

        The strategy is any object with the callback methods:
            on_book_update(ctx), on_trade(ctx, trade),
            on_own_fill(ctx, fill), on_ack(ctx, ack),
            on_reject(ctx, order_id), on_cancel(ctx, order_id),
            on_timer(ctx).

        Returns a dict with 'fills', 'equity' (dicts of numpy arrays)
        and 'stats' (dict of counters). All arrays are numpy; convert to
        pandas with e.g. pd.DataFrame(result['fills']).
        """
        return _run(self._cfg, capture_path, strategy, timer_interval_ns)


__all__ = ["Simulator", "Context"]
