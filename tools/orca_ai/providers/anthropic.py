"""Anthropic Messages API (Claude), spoken over plain HTTP so no SDK is required."""
from __future__ import annotations

import os

import requests

from ..spec import Tool
from .base import Message, Provider, ProviderError, ToolCall, Turn

ANTHROPIC_VERSION = "2023-06-01"


class AnthropicProvider(Provider):
    def __init__(self, name: str, model: str, api_key_env: str = "ANTHROPIC_API_KEY",
                 base_url: str = "https://api.anthropic.com", max_tokens: int = 4096, **_: object):
        self.name, self.model, self.max_tokens = name, model, max_tokens
        self.base_url = base_url.rstrip("/")
        self.api_key = os.environ.get(api_key_env)
        if not self.api_key:
            raise ProviderError(f"{name}: environment variable {api_key_env} is not set")

    def _wire_messages(self, messages: list[Message]) -> list[dict]:
        wire: list[dict] = []
        for m in messages:
            if m.role == "user":
                wire.append({"role": "user", "content": m.text})
            elif m.role == "assistant":
                blocks: list[dict] = []
                if m.text:
                    blocks.append({"type": "text", "text": m.text})
                blocks += [{"type": "tool_use", "id": c.id, "name": c.name, "input": c.args} for c in m.tool_calls]
                wire.append({"role": "assistant", "content": blocks})
            else:
                blocks = []
                for call, result in m.results:
                    content: list[dict] = [{"type": "text", "text": result.text}]
                    if result.image_png_b64:
                        content.append({"type": "image", "source": {
                            "type": "base64", "media_type": "image/png", "data": result.image_png_b64}})
                    blocks.append({"type": "tool_result", "tool_use_id": call.id,
                                   "content": content, "is_error": result.is_error})
                wire.append({"role": "user", "content": blocks})
        return wire

    def complete(self, system: str, messages: list[Message], tools: list[Tool]) -> Turn:
        payload = {
            "model": self.model, "max_tokens": self.max_tokens, "system": system,
            "messages": self._wire_messages(messages),
            "tools": [{"name": t.name, "description": t.description, "input_schema": t.input_schema} for t in tools],
        }
        response = requests.post(self.base_url + "/v1/messages", json=payload, timeout=180, headers={
            "x-api-key": self.api_key, "anthropic-version": ANTHROPIC_VERSION})
        if not response.ok:
            raise ProviderError(f"{self.name}: HTTP {response.status_code}: {response.text[:500]}")
        data = response.json()
        turn = Turn(stop_reason=data.get("stop_reason", ""))
        for block in data.get("content", []):
            if block["type"] == "text":
                turn.text += block["text"]
            elif block["type"] == "tool_use":
                turn.tool_calls.append(ToolCall(block["id"], block["name"], block.get("input", {})))
        return turn
