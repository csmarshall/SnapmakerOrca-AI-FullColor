"""Minimal MCP server (stdio, JSON-RPC 2.0) exposing the Remote API tools to Claude Code, Claude Desktop, etc.

Hand-rolled to avoid the `mcp` package, which needs a newer Python than the system one. Implements
initialize, tools/list and tools/call, which is all MCP tool servers need.
"""
from __future__ import annotations

import json
import sys
from typing import Any

from .agent import forbidden_keys
from .client import RemoteAPIClient, ToolResult
from .spec import load_tools

PROTOCOL_VERSION = "2024-11-05"


def serve(read_only: bool = False, allow_destructive: bool = False, advisory: bool = True) -> None:
    tools = {t.name: t for t in load_tools(read_only=read_only, advisory=advisory)}
    if not allow_destructive:
        tools = {n: t for n, t in tools.items() if not t.destructive}
    client = RemoteAPIClient()

    def reply(msg_id: Any, result: dict | None = None, error: dict | None = None) -> None:
        out = {"jsonrpc": "2.0", "id": msg_id}
        out.update({"error": error} if error else {"result": result})
        sys.stdout.write(json.dumps(out) + "\n")
        sys.stdout.flush()

    for line in sys.stdin:
        if not line.strip():
            continue
        msg = json.loads(line)
        method, msg_id = msg.get("method"), msg.get("id")
        if msg_id is None:  # notification (e.g. notifications/initialized): no reply
            continue
        if method == "initialize":
            reply(msg_id, {"protocolVersion": PROTOCOL_VERSION, "capabilities": {"tools": {}},
                           "serverInfo": {"name": "orca-ai", "version": "0.1"}})
        elif method == "tools/list":
            reply(msg_id, {"tools": [{"name": t.name, "description": t.description,
                                      "inputSchema": t.input_schema} for t in tools.values()]})
        elif method == "tools/call":
            params = msg.get("params", {})
            tool = tools.get(params.get("name"))
            if tool is None:
                reply(msg_id, error={"code": -32602, "message": f"unknown tool {params.get('name')!r}"})
                continue
            arguments = params.get("arguments") or {}
            blocked = forbidden_keys(tool, arguments)
            result = (ToolResult(f"Refused: {', '.join(blocked)} cannot be changed by the assistant.", is_error=True)
                      if blocked else client.call(tool, arguments))
            content: list[dict] = [{"type": "text", "text": result.text}]
            if result.image_png_b64:
                content.append({"type": "image", "data": result.image_png_b64, "mimeType": "image/png"})
            reply(msg_id, {"content": content, "isError": result.is_error})
        elif method == "ping":
            reply(msg_id, {})
        else:
            reply(msg_id, error={"code": -32601, "message": f"method not found: {method}"})
