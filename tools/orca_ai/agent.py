"""Provider-agnostic tool-calling loop over the Remote API."""
from __future__ import annotations

import logging
from typing import Callable

from .client import RemoteAPIClient, ToolResult
from .prompts import SYSTEM_PROMPT
from .providers import Message, Provider, ToolCall
from .spec import Tool

log = logging.getLogger("orca_ai.agent")
ConfirmFn = Callable[[Tool, dict], bool]

# Settings a model must never write: `post_process` runs arbitrary commands on the next slice, and the
# print-host keys redirect where G-code is sent / expose credentials. Text the model reads (object names,
# 3MF metadata, G-code comments) is attacker-controllable, so these are blocked in code, not by prompt.
# Mirrors is_forbidden_write_key() in RemoteAPIController.cpp: print-host keys by prefix,
# plus keys that run host commands or inject G-code into the job.
FORBIDDEN_SETTING_KEYS = frozenset({"post_process", "printer_model", "filename_format",
                                    "bbl_use_printhost", "host_type", "gcode_substitutions"})
FORBIDDEN_KEY_PREFIXES = ("print_host", "printhost_")
FORBIDDEN_KEY_SUFFIXES = ("_gcode",)


def is_forbidden_key(key: str) -> bool:
    return (key in FORBIDDEN_SETTING_KEYS or key.startswith(FORBIDDEN_KEY_PREFIXES)
            or key.endswith(FORBIDDEN_KEY_SUFFIXES))


SETTING_WRITE_TOOLS = frozenset({"putConfig", "putObjectConfig"})


def forbidden_keys(tool: Tool, args: dict) -> list[str]:
    if tool.name not in SETTING_WRITE_TOOLS:
        return []
    return sorted(k for k in (args.get("settings") or {}) if is_forbidden_key(k))


class Agent:
    def __init__(self, provider: Provider, client: RemoteAPIClient, tools: list[Tool],
                 confirm: ConfirmFn | None = None, max_steps: int = 40,
                 on_event: Callable[[str], None] = print):
        self.provider, self.client, self.max_steps, self.on_event = provider, client, max_steps, on_event
        self.tools = {t.name: t for t in tools}
        self.confirm = confirm
        self.messages: list[Message] = []

    def _run_tool(self, call: ToolCall) -> ToolResult:
        tool = self.tools.get(call.name)
        if tool is None:
            return ToolResult(f"Unknown tool {call.name!r}", is_error=True)
        blocked = forbidden_keys(tool, call.args)
        if blocked:
            return ToolResult(f"Refused: {', '.join(blocked)} cannot be changed by the assistant "
                              "(it can run commands or redirect output). Ask the user to change it in the UI.", is_error=True)
        if tool.destructive and not (self.confirm and self.confirm(tool, call.args)):
            return ToolResult("The user declined this destructive action.", is_error=True)
        self.on_event(f"  -> {call.name} {call.args}")
        result = self.client.call(tool, call.args)
        if result.is_error:
            self.on_event(f"     !! {result.text[:200]}")
        return result

    def run(self, request: str) -> str:
        """Run one user request to completion; returns the model's final text."""
        self.messages.append(Message("user", text=request))
        for _ in range(self.max_steps):
            turn = self.provider.complete(SYSTEM_PROMPT, self.messages, list(self.tools.values()))
            self.messages.append(Message("assistant", text=turn.text, tool_calls=turn.tool_calls))
            if turn.text:
                self.on_event(turn.text)
            if not turn.tool_calls:
                return turn.text
            self.messages.append(Message("tool", results=[(c, self._run_tool(c)) for c in turn.tool_calls]))
        raise RuntimeError(f"Stopped after {self.max_steps} steps without a final answer")
