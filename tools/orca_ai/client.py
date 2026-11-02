"""HTTP client for the Orca Remote API, driven by Tool definitions from spec.py."""
from __future__ import annotations

import base64
import json
import logging
import os
from dataclasses import dataclass
from typing import Any
from urllib.parse import quote

import requests

from .spec import Tool

log = logging.getLogger("orca_ai.client")
MAX_TEXT_RESULT_CHARS = 20000  # G-code and full configs are huge; models do not need them whole


@dataclass
class ToolResult:
    text: str
    image_png_b64: str | None = None
    is_error: bool = False


class RemoteAPIClient:
    def __init__(self, base_url: str | None = None, token: str | None = None, timeout: float = 30.0):
        host = os.environ.get("ORCA_API_URL", "http://127.0.0.1:13130")
        self.base = (base_url or host).rstrip("/") + "/api/v1"
        token = token or os.environ.get("ORCA_API_TOKEN")
        if not token:
            raise RuntimeError("No Remote API token: set ORCA_API_TOKEN (Preferences > Remote API shows it)")
        self.session = requests.Session()
        self.session.headers["X-Api-Token"] = token
        self.timeout = timeout

    def call(self, tool: Tool, args: dict[str, Any]) -> ToolResult:
        if tool.local is not None:
            try:
                return tool.local(args)
            except Exception as exc:  # one bad tool must not take down the agent loop or the MCP server
                return ToolResult(f"{tool.name} failed: {type(exc).__name__}: {exc}", is_error=True)
        path = tool.path
        missing = [name for name in tool.path_params if name not in args]
        if missing:
            return ToolResult(f"Missing required argument(s): {', '.join(missing)}", is_error=True)
        for name in tool.path_params:
            path = path.replace("{" + name + "}", quote(str(args[name]), safe=""))
        params = {k: args[k] for k in tool.query_params if k in args}
        body: Any = None
        if tool.body_params == ("settings",):
            body = args.get("settings", {})
        elif tool.body_params:
            body = {k: args[k] for k in tool.body_params if k in args}
        log.debug("%s %s params=%s body=%s", tool.method, path, params, body)
        try:
            # No redirects: requests strips Authorization on a cross-host redirect but not X-Api-Token.
            response = self.session.request(tool.method, self.base + path, params=params,
                                            json=body, timeout=self.timeout, allow_redirects=False)
        except requests.RequestException as exc:
            return ToolResult(f"Remote API unreachable: {exc}", is_error=True)
        succeeded = 200 <= response.status_code < 300   # requests' .ok is true for a 3xx we refused to follow
        if tool.returns_image and succeeded and response.headers.get("Content-Type", "").startswith("image/"):
            return ToolResult("Rendered image attached.", base64.b64encode(response.content).decode())
        text = response.text
        if len(text) > MAX_TEXT_RESULT_CHARS:
            text = text[:MAX_TEXT_RESULT_CHARS] + f"\n...[truncated, {len(response.text)} chars total]"
        if not succeeded:
            try:
                detail = json.dumps(response.json())
            except ValueError:
                detail = text
            if len(detail) > MAX_TEXT_RESULT_CHARS:
                detail = detail[:MAX_TEXT_RESULT_CHARS] + f"\n...[truncated, {len(detail)} chars total]"
            return ToolResult(f"HTTP {response.status_code}: {detail}", is_error=True)
        return ToolResult(text)
