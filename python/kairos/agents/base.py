"""Base class for Python arena bots.

A bot is a plain Python class with any subset of the seven callbacks.
The C++ adapter calls them with a Context snapshot; actions
(send_limit/cancel/modify) go back through the same object.

Prices are integer ticks, quantities integer lots, time integer
nanoseconds — the same units as the engine. No floats cross the boundary.
"""


class ArenaBot:
    """Subclass me, override what you need. All callbacks default to no-op."""

    def on_book_update(self, ctx):
        """Top-of-book changed (delayed by feed latency)."""

    def on_trade(self, ctx, trade):
        """A market trade printed. trade: dict(exchange_ts_ns, price_ticks,
        qty_lots, taker_is_bid)."""

    def on_own_fill(self, ctx, fill):
        """One of my orders got filled. fill: dict(order_id, side,
        price_ticks, qty_lots, is_maker)."""

    def on_ack(self, ctx, ack):
        """Exchange acknowledged my order. ack: dict(order_id,
        effective_exchange_ts)."""

    def on_reject(self, ctx, order_id):
        """My order was rejected (rate limit, position limit, ...)."""

    def on_cancel(self, ctx, order_id):
        """My cancel was confirmed."""

    def on_timer(self, ctx):
        """Periodic wake-up (default every 500ms of logical time)."""


def snapshot(ctx):
    """Flatten a Context into a plain dict for prompts/logging."""
    bid = ctx.best_bid()
    ask = ctx.best_ask()
    mid = (bid[0] + ask[0]) // 2 if bid and ask else None
    spread = (ask[0] - bid[0]) if bid and ask else None
    return {
        "now_ns": ctx.now_ns,
        "bid_px": bid[0] if bid else None,
        "bid_qty": bid[1] if bid else None,
        "ask_px": ask[0] if ask else None,
        "ask_qty": ask[1] if ask else None,
        "mid_px": mid,
        "spread_ticks": spread,
        "position_lots": ctx.position_lots,
        "cash_lots": ctx.cash_lots,
    }
