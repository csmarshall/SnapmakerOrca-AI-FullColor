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
# Mirrors is_automation_write_blocked_key() in libslic3r/PrintConfig.cpp (which the slicer enforces;
# this is the second check): print-host keys by prefix, keys that run host commands or name files, and
# every G-code template, i.e. any setting with "gcode" in its name. The slicer tells templates from
# switches by option type; this side cannot see types, so the known non-text "gcode" settings are listed.
FORBIDDEN_SETTING_KEYS = frozenset({"post_process", "printer_model", "filename_format",
                                    "bbl_use_printhost", "host_type", "gcode_substitutions",
                                    "bed_custom_texture", "bed_custom_model"})
FORBIDDEN_KEY_PREFIXES = ("print_host", "printhost_")
NON_TEXT_GCODE_KEYS = frozenset({"emit_machine_limits_to_gcode", "export_gcode", "gcode_add_line_number",
                                 "gcode_comments", "gcode_flavor", "gcode_label_objects", "gcode_skip_config_block",
                                 "gcodeviewer", "high_precision_gcode", "skip_modified_gcodes"})


def is_forbidden_key(key: str) -> bool:
    return (key in FORBIDDEN_SETTING_KEYS or key.startswith(FORBIDDEN_KEY_PREFIXES)
            or ("gcode" in key and key not in NON_TEXT_GCODE_KEYS))


SETTING_WRITE_TOOLS = frozenset({"putConfig", "putObjectConfig", "putObjectVolumeConfig"})
# Tools whose settings travel in a differently named argument.
SETTING_ARG = {"addObjectModifier": "config"}


def forbidden_keys(tool: Tool, args: dict) -> list[str]:
    if tool.name not in SETTING_WRITE_TOOLS and tool.name not in SETTING_ARG:
        return []
    arg = SETTING_ARG.get(tool.name, "settings")
    settings = args.get(arg) or {}
    if not isinstance(settings, dict):
        return [f"{arg} (must be an object of key: value)"]
    return sorted(str(k) for k in settings if not isinstance(k, str) or is_forbidden_key(k))


def vet_call(tool: Tool, args: dict) -> tuple[dict, ToolResult | None]:
    """The checks every entry point (agent loop, MCP server) applies before a call reaches the API.

    Returns (args to send, None) or (args, an error result). loadModel gets its path confined to the model
    directories and rewritten to the resolved path, so a symlink swapped in afterwards cannot redirect it.
    """
    blocked = forbidden_keys(tool, args)
    if blocked:
        return args, ToolResult(f"Refused: {', '.join(blocked)} cannot be changed by the assistant "
                                "(it can run commands or redirect output). Ask the user to change it in the UI.", is_error=True)
    if tool.name == "loadModel":
        from .advisory import LOAD_SUFFIXES, confine_to_model_dirs
        resolved, reason = confine_to_model_dirs(args.get("path"), explicit_only=True, suffixes=LOAD_SUFFIXES)
        if resolved is None:
            return args, ToolResult(f"Refused: {reason}", is_error=True)
        return {**args, "path": str(resolved)}, None
    return args, None


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
        args, refusal = vet_call(tool, call.args)
        if refusal is not None:
            return refusal
        if tool.destructive and not (self.confirm and self.confirm(tool, args)):
            return ToolResult("The user declined this destructive action.", is_error=True)
        self.on_event(f"  -> {call.name} {args}")
        result = self.client.call(tool, args)
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
