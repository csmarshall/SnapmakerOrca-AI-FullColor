"""Minimal MCP server (stdio, JSON-RPC 2.0) exposing the Remote API tools to Claude Code, Claude Desktop, etc.

Hand-rolled to avoid the `mcp` package, which needs a newer Python than the system one. Implements
initialize, tools/list and tools/call, which is all MCP tool servers need.
"""
from __future__ import annotations

import json
import os
import sys
from typing import Any

from .agent import vet_call
from .client import RemoteAPIClient, ToolResult
from .spec import load_tools

PROTOCOL_VERSION = "2024-11-05"


def serve(read_only: bool = False, allow_destructive: bool = False, advisory: bool = True,
          allow_tools: frozenset[str] = frozenset()) -> None:
    if advisory and not os.environ.get("ORCA_AI_MODEL_DIRS"):
        # MCP clients often start servers with cwd=/ or $HOME, so the cwd default would expose far too much.
        print("orca-ai: advisory analysers disabled over MCP until --model-dir or ORCA_AI_MODEL_DIRS names a directory",
              file=sys.stderr)
        advisory = False
    tools = {t.name: t for t in load_tools(read_only=read_only, advisory=advisory, export=True)}
    unknown = allow_tools - tools.keys()
    if unknown:
        print(f"orca-ai: --allow-tool names no available tool: {', '.join(sorted(unknown))}", file=sys.stderr)
    if not allow_destructive:   # one destructive tool can be unhidden by name (--allow-tool) without unhiding the others
        tools = {n: t for n, t in tools.items() if not t.destructive or n in allow_tools}
    client = RemoteAPIClient()

    def reply(msg_id: Any, result: dict | None = None, error: dict | None = None) -> None:
        out = {"jsonrpc": "2.0", "id": msg_id}
        out.update({"error": error} if error else {"result": result})
        sys.stdout.write(json.dumps(out) + "\n")
        sys.stdout.flush()

    for line in sys.stdin:
        if not line.strip():
            continue
        try:
            msg = json.loads(line)
        except ValueError:
            reply(None, error={"code": -32700, "message": "parse error"})
            continue
        if not isinstance(msg, dict):
            reply(None, error={"code": -32600, "message": "invalid request"})
            continue
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
            params = msg.get("params") if isinstance(msg.get("params"), dict) else {}
            tool = tools.get(params.get("name")) if isinstance(params.get("name"), str) else None
            if tool is None:
                reply(msg_id, error={"code": -32602, "message": f"unknown tool {params.get('name')!r}"})
                continue
            arguments = params.get("arguments") or {}
            if not isinstance(arguments, dict):
                reply(msg_id, error={"code": -32602, "message": "arguments must be an object"})
                continue
            missing = [k for k in tool.input_schema.get("required", []) if k not in arguments]
            # One malformed call must not end the stdio loop: every later call would fail until restart.
            try:
                if missing:
                    result = ToolResult(f"Missing required argument(s): {', '.join(missing)}", is_error=True)
                else:
                    arguments, refusal = vet_call(tool, arguments)
                    result = refusal if refusal is not None else client.call(tool, arguments)
            except Exception as exc:
                result = ToolResult(f"{tool.name} failed: {type(exc).__name__}: {exc}", is_error=True)
            content: list[dict] = [{"type": "text", "text": result.text}]
            if result.image_png_b64:
                content.append({"type": "image", "data": result.image_png_b64, "mimeType": "image/png"})
            reply(msg_id, {"content": content, "isError": result.is_error})
        elif method == "ping":
            reply(msg_id, {})
        else:
            reply(msg_id, error={"code": -32601, "message": f"method not found: {method}"})
