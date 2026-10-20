"""Provider configuration. API keys are never stored: entries name the environment variable holding them."""
from __future__ import annotations

import json
import os
from pathlib import Path
from typing import Any

CONFIG_PATH = Path(os.environ.get("ORCA_AI_CONFIG", Path.home() / ".config" / "orca-ai" / "config.json"))

EXAMPLE_CONFIG: dict[str, Any] = {
    "default": "claude",
    "providers": {
        "claude": {"kind": "anthropic", "model": "claude-sonnet-5-5", "api_key_env": "ANTHROPIC_API_KEY"},
        "openai": {"kind": "openai", "model": "gpt-5", "api_key_env": "OPENAI_API_KEY", "vision": True},
        "rosa": {"kind": "openai", "base_url": "http://192.168.1.10:11434/v1", "model": "<local model name>",
                 "api_key_env": None, "vision": False},
    },
}


def load_config(path: Path | None = None) -> dict[str, Any]:
    path = path or CONFIG_PATH
    if not path.exists():
        raise FileNotFoundError(
            f"No config at {path}. Create it (run `orca-ai --init-config`); example:\n"
            + json.dumps(EXAMPLE_CONFIG, indent=2))
    return json.loads(path.read_text())


def write_example(path: Path | None = None) -> Path:
    path = path or CONFIG_PATH
    if path.exists():
        raise FileExistsError(f"{path} already exists; not overwriting")
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(EXAMPLE_CONFIG, indent=2) + "\n")
    return path
