"""Derive tool definitions from docs/remote-api/openapi.yaml - the single source of truth.

Every provider schema (MCP, Anthropic, OpenAI function calling) is generated from the
operations loaded here; nothing about the Remote API is declared twice.
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Callable

import yaml

DEFAULT_SPEC = Path(__file__).resolve().parents[2] / "docs" / "remote-api" / "openapi.yaml"
HTTP_METHODS = ("get", "post", "put", "delete")
# The event stream is a WebSocket upgrade, not a request/response operation.
EXCLUDED_OPERATIONS = {"events"}


@dataclass(frozen=True)
class Tool:
    name: str
    description: str
    method: str
    path: str
    input_schema: dict[str, Any]
    path_params: tuple[str, ...] = ()
    query_params: tuple[str, ...] = ()
    body_params: tuple[str, ...] = ()
    mutating: bool = False
    destructive: bool = False
    returns_image: bool = False
    returns_text: bool = False
    raw: dict[str, Any] = field(default_factory=dict, compare=False, repr=False)
    # Tools that do not go through the Remote API (e.g. advisory analysers) carry their own executor.
    local: Callable[[dict[str, Any]], Any] | None = field(default=None, compare=False, repr=False)


def _resolve(doc: dict[str, Any], node: Any) -> Any:
    """Follow local $ref pointers (and merge allOf) so schemas are self-contained."""
    if isinstance(node, dict):
        if "$ref" in node:
            target: Any = doc
            for part in node["$ref"].lstrip("#/").split("/"):
                target = target[part]
            return _resolve(doc, target)
        if "allOf" in node:
            merged: dict[str, Any] = {"type": "object", "properties": {}, "required": []}
            for sub in node["allOf"]:
                sub = _resolve(doc, sub)
                merged["properties"].update(sub.get("properties", {}))
                merged["required"].extend(sub.get("required", []))
            return merged
        return {k: _resolve(doc, v) for k, v in node.items()}
    if isinstance(node, list):
        return [_resolve(doc, v) for v in node]
    return node


def _content_kinds(op: dict[str, Any]) -> set[str]:
    kinds: set[str] = set()
    for response in (op.get("responses") or {}).values():
        kinds.update((response or {}).get("content", {}).keys())
    return kinds


def load_tools(spec_path: str | os.PathLike[str] | None = None, read_only: bool = False,
               advisory: bool = False, export: bool = False) -> list[Tool]:
    path = Path(spec_path or os.environ.get("ORCA_AI_SPEC") or DEFAULT_SPEC)
    doc = yaml.safe_load(path.read_text())
    tools: list[Tool] = []
    for url_path, item in doc["paths"].items():
        for method in HTTP_METHODS:
            op = item.get(method)
            if op is None or op["operationId"] in EXCLUDED_OPERATIONS:
                continue
            op = _resolve(doc, op)
            mutating = bool(op.get("x-mutating")) or method in ("post", "put", "delete") and op["operationId"] not in READ_ONLY_POSTS
            if read_only and mutating:
                continue
            properties: dict[str, Any] = {}
            required: list[str] = []
            path_params, query_params = [], []
            for param in op.get("parameters") or []:
                schema = dict(param.get("schema", {}))
                if "description" in param:
                    schema["description"] = param["description"]
                properties[param["name"]] = schema
                if param.get("required"):
                    required.append(param["name"])
                (path_params if param["in"] == "path" else query_params).append(param["name"])
            body_params: list[str] = []
            body_schema = (op.get("requestBody", {}).get("content", {}).get("application/json", {}) or {}).get("schema")
            if body_schema is not None:
                body_params = list(body_schema.get("properties", {}))
                if body_params:
                    properties.update(body_schema["properties"])
                    required.extend(body_schema.get("required", []))
                else:  # free-form object body (e.g. config edits): one object argument
                    properties["settings"] = {
                        "type": "object",
                        "description": "Key/value map sent as the request body.",
                        "additionalProperties": True,
                    }
                    required.append("settings")
                    body_params = ["settings"]
            kinds = _content_kinds(op)
            description = op.get("summary", op["operationId"])
            if op.get("description"):
                description += ". " + " ".join(op["description"].split())
            tools.append(Tool(
                name=op["operationId"],
                description=description,
                method=method.upper(),
                path=url_path,
                input_schema={"type": "object", "properties": properties, "required": sorted(set(required))},
                path_params=tuple(path_params),
                query_params=tuple(query_params),
                body_params=tuple(body_params),
                mutating=mutating,
                destructive=bool(op.get("x-destructive")),
                returns_image="image/png" in kinds,
                returns_text="text/plain" in kinds,
                raw=op,
            ))
    if advisory:
        from .advisory import advisory_tools
        tools += advisory_tools()
    if export and not read_only:
        from .export import export_tools
        tools += export_tools()
    return tools


# POST operations that only compute or read, and so are not "mutating".
READ_ONLY_POSTS = {"getPresetConfig"}
