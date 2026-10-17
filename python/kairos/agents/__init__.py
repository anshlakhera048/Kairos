"""Python agent framework for the Kairos arena.

Write a bot by subclassing :class:`ArenaBot` and overriding the callbacks
you need; run it with ``kairos arena --py-bot mybot.py:MyBot``.

The market runs in logical time on the deterministic C++ engine; your bot
thinks in wall-clock time under a per-callback budget (see ``--budget-s``).
Thinking longer buys no market advantage — only your patience.
"""

from .base import ArenaBot, snapshot
from .debate import DebateTrader
from .llm import LLMClient, MockLLM

__all__ = ["ArenaBot", "snapshot", "DebateTrader", "LLMClient", "MockLLM"]
