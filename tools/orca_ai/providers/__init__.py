"""Provider registry: config entries name a `kind`; adding a backend means adding one class here."""
from __future__ import annotations

from typing import Any

from .anthropic import AnthropicProvider
from .base import Message, Provider, ProviderError, ToolCall, Turn
from .openai_compat import OpenAICompatProvider

KINDS: dict[str, type[Provider]] = {
    "anthropic": AnthropicProvider,
    "openai": OpenAICompatProvider,  # OpenAI itself and every OpenAI-compatible server
}


def build_provider(name: str, entry: dict[str, Any]) -> Provider:
    kind = entry.get("kind")
    if kind not in KINDS:
        raise ProviderError(f"provider '{name}': unknown kind {kind!r} (known: {', '.join(KINDS)})")
    options = {k: v for k, v in entry.items() if k != "kind"}
    return KINDS[kind](name=name, **options)


__all__ = ["Message", "Provider", "ProviderError", "ToolCall", "Turn", "build_provider", "KINDS"]
