#!/usr/bin/env python3
"""Unit tests for the Python agent framework (no engine needed).

Run from the repo root:
    PYTHONPATH=python python3 python/test_agents.py
"""

import sys

sys.path.insert(0, "python")

from kairos.agents import ArenaBot, DebateTrader, MockLLM, snapshot
from kairos.agents.llm import LLMError


class FakeCtx:
    """Records order actions; enough of the Context API for agent tests."""

    def __init__(self, now_ns=0, bid=(9998, 50_000), ask=(10002, 50_000),
                 position_lots=0):
        self.now_ns = now_ns
        self._bid = bid
        self._ask = ask
        self.position_lots = position_lots
        self.cash_lots = 0
        self.sent = []       # (side, px, qty, post_only, id)
        self.cancelled = []  # ids
        self._next_id = 1

    def best_bid(self):
        return self._bid

    def best_ask(self):
        return self._ask

    def equity_lots(self, mark_px):
        return self.cash_lots + self.position_lots * mark_px

    def send_limit(self, side, price_ticks, qty_lots, post_only=True):
        oid = self._next_id
        self._next_id += 1
        self.sent.append((side, price_ticks, qty_lots, post_only, oid))
        return oid

    def cancel(self, order_id):
        self.cancelled.append(order_id)

    def modify(self, order_id, px, qty):
        return order_id


def test_snapshot():
    ctx = FakeCtx(now_ns=123, position_lots=7_000)
    s = snapshot(ctx)
    assert s["mid_px"] == 10000 and s["spread_ticks"] == 4
    assert s["position_lots"] == 7_000 and s["now_ns"] == 123
    print("snapshot: OK")


def test_base_callbacks_noop():
    bot, ctx = ArenaBot(), FakeCtx()
    bot.on_book_update(ctx)
    bot.on_trade(ctx, {})
    bot.on_own_fill(ctx, {})
    bot.on_ack(ctx, {})
    bot.on_reject(ctx, 1)
    bot.on_cancel(ctx, 1)
    bot.on_timer(ctx)
    print("base no-op callbacks: OK")


def test_debate_quotes_with_mock():
    bot = DebateTrader(llm="mock", decision_interval_ns=30_000_000_000)
    ctx = FakeCtx(now_ns=0)
    bot.on_timer(ctx)  # first decision
    assert len(ctx.sent) == 2, f"expected 2 quotes, got {ctx.sent}"
    sides = {s[0] for s in ctx.sent}
    assert sides == {"bid", "ask"}
    assert all(s[3] for s in ctx.sent), "quotes must be post-only"
    assert ctx.sent[0][1] == 9998 and ctx.sent[1][1] == 10002
    assert bot.stats["decisions"] == 1 and bot.stats["llm_calls"] == 4
    # Second timer inside the interval: no new decision.
    bot.on_timer(ctx)
    assert len(ctx.sent) == 2
    # After the interval: re-quotes (cancels stale first).
    ctx2 = FakeCtx(now_ns=31_000_000_000)
    bot.on_timer(ctx2)
    assert len(ctx2.cancelled) == 2 and len(ctx2.sent) == 2
    print("debate mock quoting: OK")


def test_debate_hold_on_llm_error():
    class Broken:
        def chat(self, *a, **k):
            raise LLMError("boom")

    bot = DebateTrader(llm=Broken())
    ctx = FakeCtx(now_ns=0)
    bot.on_timer(ctx)
    assert ctx.sent == [] and bot.stats["errors"] == 1
    print("debate fail-safe hold: OK")


def test_risk_gate_blocks_oversize():
    script = ["a", "b", "c",
              '{"action": "quote", "bid_offset_ticks": 2, '
              '"ask_offset_ticks": 2, "qty_lots": 999999999}']
    bot = DebateTrader(llm=MockLLM(script=script),
                       order_qty_lots=100_000, max_position_lots=5_000_000)
    ctx = FakeCtx(now_ns=0)
    bot.on_timer(ctx)
    assert ctx.sent == [], "risk gate must block oversized qty"
    assert bot.stats["errors"] == 1
    print("debate risk gate: OK")


def test_debate_flatten():
    script = ["a", "b", "c", '{"action": "flatten"}']
    bot = DebateTrader(llm=MockLLM(script=script))
    ctx = FakeCtx(now_ns=0, position_lots=300_000)
    bot.on_timer(ctx)
    assert len(ctx.sent) == 1
    side, px, qty, post_only, _ = ctx.sent[0]
    assert side == "ask" and not post_only and qty == 100_000
    assert px == 9997  # one tick through the bid
    print("debate flatten: OK")


if __name__ == "__main__":
    test_snapshot()
    test_base_callbacks_noop()
    test_debate_quotes_with_mock()
    test_debate_hold_on_llm_error()
    test_risk_gate_blocks_oversize()
    test_debate_flatten()
    print("all agent tests passed")
