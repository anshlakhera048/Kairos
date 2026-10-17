"""Reference LLM trading agent: analyst -> bull/bear debate -> trader -> risk gate.

The pattern mirrors multi-agent trading frameworks (analysts debate, a
trader decides, risk has the last word) — except here the trader's orders
hit a real deterministic matching engine, not a toy simulator.

Usage:
    from kairos.agents import DebateTrader
    bot = DebateTrader()                      # MockLLM unless a key is set
    bot = DebateTrader(llm="live")             # real LLM via env config

    kairos arena --py-bot mybot.py:make_bot   # with `def make_bot(): ...`

Fail-safe: any LLM error, bad JSON, or risk-gate breach -> hold position
(cancel nothing, send nothing). The agent never trades what it can't parse.
"""

import json
import os

from .base import ArenaBot, snapshot
from .llm import LLMClient, LLMError, MockLLM

ANALYST_SYS = (
    "You are a market microstructure analyst. Given a market snapshot, "
    "summarize the state in 2-3 sentences: trend, spread, liquidity, "
    "inventory risk. Be terse."
)
BULL_SYS = (
    "You are the bull on a trading desk. Argue ONLY the bullish case for "
    "the next 30 seconds: why the mid might rise. 2 sentences max."
)
BEAR_SYS = (
    "You are the bear on a trading desk. Argue ONLY the bearish case for "
    "the next 30 seconds: why the mid might fall. 2 sentences max."
)
TRADER_SYS = (
    "You are a market-making trader. Given the analyst summary and the "
    "bull/bear debate plus a market snapshot, choose ONE action.\n"
    "Prices are integer ticks, quantities integer lots. Offsets are in "
    "ticks away from the mid (must be >= 1).\n"
    "Reply with EXACTLY one JSON object, no other text:\n"
    '{"action": "quote", "bid_offset_ticks": 2, "ask_offset_ticks": 2, "qty_lots": 100000}\n'
    'or {"action": "hold"} or {"action": "flatten"}\n'
    '"quote" posts a passive bid AND ask around the mid. "flatten" closes '
    "inventory aggressively. Prefer quoting when the spread covers your risk."
)


def _extract_json(text):
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end <= start:
        raise LLMError(f"no JSON object in reply: {text[:120]!r}")
    return json.loads(text[start:end + 1])


class DebateTrader(ArenaBot):
    """Bull/bear debate agent with a rule-based risk gate."""

    def __init__(self, llm=None, decision_interval_ns=30_000_000_000,
                 order_qty_lots=100_000, max_position_lots=5_000_000,
                 max_spread_ticks=50):
        if llm is None or llm == "auto":
            self.llm = (LLMClient() if os.environ.get("KAIROS_LLM_API_KEY")
                        else MockLLM())
        elif llm == "live":
            self.llm = LLMClient()
        elif llm == "mock":
            self.llm = MockLLM()
        else:
            self.llm = llm
        self.live = isinstance(self.llm, LLMClient)
        self.decision_interval_ns = decision_interval_ns
        self.order_qty_lots = order_qty_lots
        self.max_position_lots = max_position_lots
        self.max_spread_ticks = max_spread_ticks

        self._last_decision_ns = -10**18
        self._open_ids = []
        self.stats = {"decisions": 0, "llm_calls": 0, "errors": 0,
                      "orders_sent": 0}

    # -- ArenaBot callbacks ------------------------------------------------

    def on_timer(self, ctx):
        if ctx.now_ns - self._last_decision_ns < self.decision_interval_ns:
            return
        self._last_decision_ns = ctx.now_ns
        snap = snapshot(ctx)
        if snap["mid_px"] is None:
            return
        try:
            action = self._decide(snap)
        except (LLMError, ValueError, KeyError):
            self.stats["errors"] += 1
            return  # fail-safe: hold
        self.stats["decisions"] += 1
        self._act(ctx, snap, action)

    def on_own_fill(self, ctx, fill):
        oid = fill["order_id"]
        if oid in self._open_ids:
            self._open_ids.remove(oid)

    def on_cancel(self, ctx, order_id):
        if order_id in self._open_ids:
            self._open_ids.remove(order_id)

    # -- Decision pipeline ---------------------------------------------------

    def _ask(self, system, user):
        self.stats["llm_calls"] += 1
        return self.llm.chat(system, user)

    def _decide(self, snap):
        snap_json = json.dumps(snap)
        analyst = self._ask(ANALYST_SYS, snap_json)
        bull = self._ask(BULL_SYS, analyst)
        bear = self._ask(BEAR_SYS, analyst)
        raw = self._ask(TRADER_SYS, json.dumps({
            "analyst": analyst, "bull": bull, "bear": bear,
            "snapshot": snap_json}))
        action = _extract_json(raw)
        if action.get("action") not in ("quote", "hold", "flatten"):
            raise ValueError(f"bad action: {action!r}")
        return action

    # -- Risk gate + execution -----------------------------------------------

    def _risk_ok(self, snap, action):
        if action["action"] != "quote":
            return True
        qty = int(action.get("qty_lots", 0))
        bid_off = int(action.get("bid_offset_ticks", 0))
        ask_off = int(action.get("ask_offset_ticks", 0))
        return (0 < qty <= self.order_qty_lots
                and abs(snap["position_lots"]) + qty <= self.max_position_lots
                and 1 <= bid_off <= self.max_spread_ticks
                and 1 <= ask_off <= self.max_spread_ticks)

    def _cancel_all(self, ctx):
        for oid in list(self._open_ids):
            ctx.cancel(oid)
        self._open_ids.clear()

    def _act(self, ctx, snap, action):
        kind = action["action"]
        if kind == "hold":
            return
        if kind == "flatten":
            self._cancel_all(ctx)
            pos = snap["position_lots"]
            if pos == 0 or snap["mid_px"] is None:
                return
            # Aggressive limit through the touch to close inventory.
            if pos > 0:
                px = snap["bid_px"] - 1  # sell into the bid
                oid = ctx.send_limit("ask", px, min(pos, self.order_qty_lots),
                                     post_only=False)
            else:
                px = snap["ask_px"] + 1  # buy into the ask
                oid = ctx.send_limit("bid", px, min(-pos, self.order_qty_lots),
                                     post_only=False)
            self._open_ids.append(oid)
            self.stats["orders_sent"] += 1
            return
        # quote
        if not self._risk_ok(snap, action):
            self.stats["errors"] += 1
            return
        self._cancel_all(ctx)
        mid = snap["mid_px"]
        qty = int(action["qty_lots"])
        bid_px = mid - int(action["bid_offset_ticks"])
        ask_px = mid + int(action["ask_offset_ticks"])
        self._open_ids.append(
            ctx.send_limit("bid", bid_px, qty, post_only=True))
        self._open_ids.append(
            ctx.send_limit("ask", ask_px, qty, post_only=True))
        self.stats["orders_sent"] += 2
