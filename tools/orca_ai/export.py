"""Local tool that saves the last slice's G-code to a file, so the assistant never has to pull megabytes of text through chat.

Writes only inside the directory named by --export-dir / ORCA_AI_EXPORT_DIR, never overwrites, and refuses path-like names.
Nothing is sent to a printer: this is the same G-code `getGcode` returns, written to disk.
"""
from __future__ import annotations

import logging
import os
import re
from pathlib import Path
from typing import Any

import requests

from .client import ToolResult
from .spec import Tool

log = logging.getLogger("orca_ai.export")
EXPORT_DIR_ENV = "ORCA_AI_EXPORT_DIR"
SAFE_NAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._ -]{0,119}$")
GCODE_SUFFIX = ".gcode"
CHUNK_BYTES = 1 << 20


def export_dir() -> Path | None:
    """The configured output directory, or None when export is not enabled."""
    value = os.environ.get(EXPORT_DIR_ENV)
    return Path(value).expanduser().resolve() if value else None


def _target(directory: Path, filename: str) -> Path:
    """A fresh path under `directory` for `filename`; raises ValueError for anything that is not a plain file name."""
    name = filename if filename.lower().endswith(GCODE_SUFFIX) else filename + GCODE_SUFFIX
    if not SAFE_NAME.match(name) or ".." in name:
        raise ValueError("filename must be a plain name (letters, digits, space, . _ -), no folders")
    path = directory / name
    if path.resolve().parent != directory:
        raise ValueError("filename resolves outside the export directory")
    stem, counter = path.stem, 1
    while path.exists():                       # never overwrite an earlier export
        path = directory / f"{stem}-{counter}{GCODE_SUFFIX}"
        counter += 1
    return path


def save_gcode(args: dict[str, Any]) -> ToolResult:
    """Stream GET /gcode from the running slicer into a new file in the export directory."""
    directory = export_dir()
    if directory is None:
        return ToolResult(f"Export is not enabled: start the server with --export-dir (or set {EXPORT_DIR_ENV}).", is_error=True)
    directory.mkdir(parents=True, exist_ok=True)
    try:
        path = _target(directory, str(args["filename"]))
    except (ValueError, KeyError) as exc:
        return ToolResult(str(exc), is_error=True)
    base = os.environ.get("ORCA_API_URL", "http://127.0.0.1:13130").rstrip("/") + "/api/v1"
    with requests.get(base + "/gcode", headers={"X-Api-Token": os.environ["ORCA_API_TOKEN"]}, stream=True, timeout=120) as response:
        if not response.ok:
            return ToolResult(f"HTTP {response.status_code}: {response.text[:300]} (slice first, then export)", is_error=True)
        written = 0
        with open(path, "xb") as out:          # "x": fail rather than overwrite if the name was taken meanwhile
            for chunk in response.iter_content(CHUNK_BYTES):
                out.write(chunk)
                written += len(chunk)
    log.info("saved %d bytes of G-code to %s", written, path)
    return ToolResult(f"Saved {written} bytes to {path}")


def export_tools() -> list[Tool]:
    """The export tools, or none when no export directory is configured."""
    if export_dir() is None:
        return []
    return [Tool(
        name="saveGcode",
        description=("Save the G-code of the last successful slice to a new .gcode file in the configured export directory "
                     "(never overwrites; returns the path). Does not send anything to a printer."),
        method="LOCAL", path="saveGcode", mutating=True,
        input_schema={"type": "object", "required": ["filename"],
                      "properties": {"filename": {"type": "string", "description": "Plain file name, e.g. DFLH_test1 (.gcode is added)."}}},
        local=save_gcode)]
