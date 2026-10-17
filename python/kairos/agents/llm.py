"""Minimal LLM plumbing for arena bots. Stdlib only — no new dependencies.

LLMClient speaks the OpenAI-compatible ``/chat/completions`` API, so it
works with OpenAI, OpenRouter, Ollama, or any compatible gateway:

    export KAIROS_LLM_API_BASE="https://openrouter.ai/api/v1"
    export KAIROS_LLM_API_KEY="..."
    export KAIROS_LLM_MODEL="anthropic/claude-haiku-4-5"

MockLLM returns deterministic canned replies for offline tests and demos.
"""

import json
import os
import urllib.request


class LLMError(RuntimeError):
    pass


class LLMClient:
    """Thin OpenAI-compatible chat client over stdlib urllib."""

    def __init__(self, api_base=None, api_key=None, model=None, timeout_s=60):
        self.api_base = (
            api_base or os.environ.get("KAIROS_LLM_API_BASE", "https://api.openai.com/v1")
        ).rstrip("/")
        self.api_key = api_key or os.environ.get("KAIROS_LLM_API_KEY")
        self.model = model or os.environ.get("KAIROS_LLM_MODEL", "gpt-4o-mini")
        self.timeout_s = timeout_s

    def chat(self, system, user, max_tokens=512, temperature=0.2):
        """Return the assistant's reply text. Raises LLMError on failure."""
        if not self.api_key:
            raise LLMError(
                "no API key: set KAIROS_LLM_API_KEY (or pass api_key=)")
        body = json.dumps({
            "model": self.model,
            "messages": [
                {"role": "system", "content": system},
                {"role": "user", "content": user},
            ],
            "max_tokens": max_tokens,
            "temperature": temperature,
        }).encode()
        req = urllib.request.Request(
            self.api_base + "/chat/completions", data=body, method="POST",
            headers={"Content-Type": "application/json",
                     "Authorization": f"Bearer {self.api_key}"})
        try:
            with urllib.request.urlopen(req, timeout=self.timeout_s) as resp:
                payload = json.load(resp)
        except Exception as e:
            raise LLMError(f"LLM request failed: {e}") from e
        try:
            return payload["choices"][0]["message"]["content"]
        except (KeyError, IndexError, TypeError) as e:
            raise LLMError(f"unexpected LLM response shape: {e}") from e


class MockLLM:
    """Deterministic stand-in for offline tests and demos.

    Replies cycle through ``script``; each entry is returned verbatim.
    Default script makes the debate agent quote passively around the mid.
    """

    DEFAULT_SCRIPT = [
        # analyst
        "Market is calm. Spread is tight, no clear direction.",
        # bull
        "Bull case: buying pressure could lift the mid a few ticks.",
        # bear
        "Bear case: no momentum; mean reversion likely.",
        # trader decision
        '{"action": "quote", "bid_offset_ticks": 2, "ask_offset_ticks": 2, "qty_lots": 100000}',
    ]

    def __init__(self, script=None):
        self.script = list(script or self.DEFAULT_SCRIPT)
        self.calls = 0

    def chat(self, system, user, max_tokens=512, temperature=0.2):
        reply = self.script[self.calls % len(self.script)]
        self.calls += 1
        return reply
