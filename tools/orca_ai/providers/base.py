"""Provider-neutral conversation types and the interface every model backend implements."""
from __future__ import annotations

from abc import ABC, abstractmethod
from dataclasses import dataclass, field
from typing import Any

from ..client import ToolResult
from ..spec import Tool


@dataclass
class ToolCall:
    id: str
    name: str
    args: dict[str, Any]


@dataclass
class Turn:
    """One assistant response: free text and/or tool calls."""
    text: str = ""
    tool_calls: list[ToolCall] = field(default_factory=list)
    stop_reason: str = ""


@dataclass
class Message:
    role: str  # "user" | "assistant" | "tool"
    text: str = ""
    tool_calls: list[ToolCall] = field(default_factory=list)
    results: list[tuple[ToolCall, ToolResult]] = field(default_factory=list)


class Provider(ABC):
    name: str

    @abstractmethod
    def complete(self, system: str, messages: list[Message], tools: list[Tool]) -> Turn: ...


class ProviderError(RuntimeError):
    pass
