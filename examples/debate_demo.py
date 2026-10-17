"""Run the reference LLM debate agent on the Kairos arena.

Offline (mock LLM, deterministic):
    kairos arena --bots none --py-bot examples/debate_demo.py:make_debate \\
        --seeds 3 --duration 60

Live (any OpenAI-compatible endpoint):
    export KAIROS_LLM_API_BASE="https://openrouter.ai/api/v1"
    export KAIROS_LLM_API_KEY="..."
    export KAIROS_LLM_MODEL="anthropic/claude-haiku-4-5"
    kairos arena --bots as --py-bot examples/debate_demo.py:make_debate \\
        --seeds 3 --duration 120 --budget-s 300

Compare the LLM agent against the C++ Avellaneda-Stoikov market maker on
identical seeds and markets.
"""

from kairos.agents import DebateTrader


def make_debate():
    """Zero-arg factory: the arena creates a fresh bot per seed."""
    return DebateTrader(
        llm="auto",  # live client if KAIROS_LLM_API_KEY is set, else mock
        decision_interval_ns=30_000_000_000,
        order_qty_lots=100_000,
        max_position_lots=5_000_000,
    )
