"""Any OpenAI-compatible /chat/completions endpoint: OpenAI, Ollama, vLLM, llama.cpp server, LM Studio, ..."""
from __future__ import annotations

import json
import os

import requests

from ..spec import Tool
from .base import Message, Provider, ProviderError, ToolCall, Turn


class OpenAICompatProvider(Provider):
    def __init__(self, name: str, model: str, base_url: str = "https://api.openai.com/v1",
                 api_key_env: str | None = "OPENAI_API_KEY", vision: bool = False,
                 max_tokens: int = 4096, **_: object):
        self.name, self.model, self.vision, self.max_tokens = name, model, vision, max_tokens
        self.base_url = base_url.rstrip("/")
        # Local servers usually need no key; a configured-but-missing key is still an error.
        self.api_key = os.environ.get(api_key_env) if api_key_env else None
        if api_key_env and not self.api_key:
            raise ProviderError(f"{name}: environment variable {api_key_env} is not set")

    def _wire_messages(self, system: str, messages: list[Message]) -> list[dict]:
        wire: list[dict] = [{"role": "system", "content": system}]
        for m in messages:
            if m.role == "user":
                wire.append({"role": "user", "content": m.text})
            elif m.role == "assistant":
                entry: dict = {"role": "assistant", "content": m.text or None}
                if m.tool_calls:
                    entry["tool_calls"] = [{"id": c.id, "type": "function",
                                            "function": {"name": c.name, "arguments": json.dumps(c.args)}}
                                           for c in m.tool_calls]
                wire.append(entry)
            else:
                images: list[str] = []
                for call, result in m.results:
                    text = result.text
                    if result.image_png_b64:
                        if self.vision:
                            images.append(result.image_png_b64)
                            text += " (image follows in the next message)"
                        else:
                            text += " (image omitted: this provider is configured without vision)"
                    wire.append({"role": "tool", "tool_call_id": call.id, "content": text})
                if images:  # tool messages cannot carry images in this API; attach as a user turn
                    wire.append({"role": "user", "content": [
                        {"type": "image_url", "image_url": {"url": "data:image/png;base64," + b}} for b in images]})
        return wire

    def complete(self, system: str, messages: list[Message], tools: list[Tool]) -> Turn:
        payload = {
            "model": self.model, "max_tokens": self.max_tokens,
            "messages": self._wire_messages(system, messages),
            "tools": [{"type": "function", "function": {
                "name": t.name, "description": t.description, "parameters": t.input_schema}} for t in tools],
        }
        headers = {"Authorization": f"Bearer {self.api_key}"} if self.api_key else {}
        response = requests.post(self.base_url + "/chat/completions", json=payload, headers=headers, timeout=300)
        if not response.ok:
            raise ProviderError(f"{self.name}: HTTP {response.status_code}: {response.text[:500]}")
        choice = response.json()["choices"][0]
        message = choice["message"]
        turn = Turn(text=message.get("content") or "", stop_reason=choice.get("finish_reason", ""))
        for call in message.get("tool_calls") or []:
            try:
                args = json.loads(call["function"]["arguments"] or "{}")
            except json.JSONDecodeError:
                raise ProviderError(f"{self.name}: model returned malformed tool arguments: "
                                    f"{call['function']['arguments'][:200]}")
            turn.tool_calls.append(ToolCall(call["id"], call["function"]["name"], args))
        return turn
