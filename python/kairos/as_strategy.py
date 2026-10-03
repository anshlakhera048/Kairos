"""Avellaneda-Stoikov market maker in Python (Phase 3C).

Mirrors strategies/avellaneda_stoikov.hpp. Same formulas, same behavior;
use it to validate the C++ implementation or to prototype parameter
changes quickly (then port the good ones to C++ for speed).

The model (see include/kairos/as_model.hpp for the full derivation):
    reservation price: r = s - q * gamma * sigma^2 * T
    optimal spread:    d = (2/gamma) * ln(1 + gamma/k) + gamma * sigma^2 * T
    bid = r - d/2, ask = r + d/2   (rounded to ticks, clamped to touch)

All prices in ticks, quantities in lots, time in seconds.
"""

import math


class ASModel:
    """Pure A-S quote math (no state). Matches include/kairos/as_model.hpp."""

    def __init__(self, gamma, sigma, kappa, horizon_s):
        self.gamma = gamma
        self.sigma = sigma
        self.kappa = kappa
        self.horizon_s = horizon_s

    def reservation_price(self, mid_ticks, inventory_lots):
        """r = s - q * gamma * sigma^2 * T (in ticks, float)."""
        return (mid_ticks - inventory_lots * self.gamma
                * self.sigma ** 2 * self.horizon_s)

    def quotes(self, mid_ticks, inventory_lots, elapsed_s=0.0):
        """(bid_ticks, ask_ticks) as integers.

        Uses the inventory-asymmetric distances from Avellaneda-Stoikov:
            d_bid = (1/g)ln(1+g/k) + g*sigma^2*T*(q+0.5)
            d_ask = (1/g)ln(1+g/k) + g*sigma^2*T*(-q+0.5)
        Distances are clamped to >= 1 tick; bid < ask is enforced.
        """
        t_rem = max(self.horizon_s - elapsed_s, 0.0)
        q = float(inventory_lots)
        s = float(mid_ticks)
        r = s - q * self.gamma * self.sigma ** 2 * t_rem

        base = math.log(1.0 + self.gamma / self.kappa) / self.gamma
        adj = self.gamma * self.sigma ** 2 * t_rem
        d_bid = max(base + adj * (q + 0.5), 1.0)
        d_ask = max(base + adj * (-q + 0.5), 1.0)

        bid = int(round(r - d_bid))
        ask = int(round(r + d_ask))
        if bid >= ask:
            ask = bid + 1
        return (bid, ask)

    @staticmethod
    def estimate_sigma(mid_ticks_series, dt_s):
        """Realized volatility per sqrt(second) from mid prices.

        sigma = std(log returns) / sqrt(dt). Needs >= 2 points.
        """
        if len(mid_ticks_series) < 2:
            return 0.0
        rets = [math.log(mid_ticks_series[i] / mid_ticks_series[i - 1])
                for i in range(1, len(mid_ticks_series))
                if mid_ticks_series[i - 1] > 0]
        if len(rets) < 2:
            return 0.0
        mean = sum(rets) / len(rets)
        var = sum((x - mean) ** 2 for x in rets) / (len(rets) - 1)
        return math.sqrt(var / dt_s) if var > 0 else 0.0

    @staticmethod
    def estimate_kappa(trade_distances_ticks):
        """Fit exponential: mean distance = 1/kappa, so kappa = 1/mean."""
        if not trade_distances_ticks:
            return 0.0
        mean = sum(trade_distances_ticks) / len(trade_distances_ticks)
        return 1.0 / mean if mean > 0 else 0.0


class AvellanedaStoikov:
    """Reference A-S market-making strategy (Python).

    Quotes both sides at the touch (clamped from A-S optimal), requotes
    on book updates and on a timer, post-only, with inventory limits and
    a fast-market spread guard. Matches the C++ ASStrategy behavior.
    """

    def __init__(self, gamma=1500.0, sigma=0.0002, kappa=2.0,
                 horizon_s=3600.0, order_size_lots=1_000_000,
                 max_inventory_lots=10_000_000, max_spread_ticks=100,
                 requote_on_trade=True):
        self.model = ASModel(gamma, sigma, kappa, horizon_s)
        self.order_size_lots = order_size_lots
        self.max_inventory_lots = max_inventory_lots
        self.max_spread_ticks = max_spread_ticks
        self.requote_on_trade = requote_on_trade
        self._bid_id = None
        self._ask_id = None

    # -- internal ------------------------------------------------------
    def _refresh(self, ctx):
        bb = ctx.best_bid()
        ba = ctx.best_ask()
        if bb is None or ba is None:
            return
        bid_touch, ask_touch = bb[0], ba[0]
        if ask_touch - bid_touch > self.max_spread_ticks:
            return  # fast market: stand aside

        mid = (bid_touch + ask_touch) // 2
        inv = ctx.position_lots
        q_bid, q_ask = self.model.quotes(mid, inv)
        # Clamp to the touch for backtest alignment (synthetic trades only
        # print where the historical book had liquidity).
        q_bid = min(q_bid, bid_touch)
        q_ask = max(q_ask, ask_touch)

        # Cancel stale quotes.
        if self._bid_id is not None:
            ctx.cancel(self._bid_id)
            self._bid_id = None
        if self._ask_id is not None:
            ctx.cancel(self._ask_id)
            self._ask_id = None

        # Inventory limits: only quote the side that reduces |inventory|.
        if inv < self.max_inventory_lots:
            self._bid_id = ctx.send_limit("bid", q_bid, self.order_size_lots,
                                          post_only=True)
        if inv > -self.max_inventory_lots:
            self._ask_id = ctx.send_limit("ask", q_ask, self.order_size_lots,
                                          post_only=True)

    # -- strategy callbacks --------------------------------------------
    def on_book_update(self, ctx):
        self._refresh(ctx)

    def on_trade(self, ctx, trade):
        if self.requote_on_trade:
            self._refresh(ctx)

    def on_own_fill(self, ctx, fill):
        pass

    def on_ack(self, ctx, ack):
        pass

    def on_reject(self, ctx, order_id):
        if order_id == self._bid_id:
            self._bid_id = None
        if order_id == self._ask_id:
            self._ask_id = None

    def on_cancel(self, ctx, order_id):
        pass

    def on_timer(self, ctx):
        self._refresh(ctx)
