"""orca-ai: natural-language control of a running OrcaSlicer via its Remote API."""
from __future__ import annotations

import argparse
import logging
import os
import sys

from .agent import Agent
from .client import RemoteAPIClient
from .config import CONFIG_PATH, load_config, write_example
from .providers import build_provider
from .spec import Tool, load_tools


def confirm_on_terminal(tool: Tool, args: dict) -> bool:
    return input(f"Allow DESTRUCTIVE action {tool.name} {args}? [y/N] ").strip().lower() == "y"


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="orca-ai", description=__doc__)
    ap.add_argument("request", nargs="*", help="what you want done, in plain language")
    ap.add_argument("-p", "--provider", help="provider name from the config (default: config 'default')")
    ap.add_argument("--read-only", action="store_true", help="expose only tools that do not change anything")
    ap.add_argument("--yes", action="store_true", help="auto-approve destructive actions (preset deletion)")
    ap.add_argument("--list-tools", action="store_true", help="print the tools derived from the OpenAPI spec and exit")
    ap.add_argument("--init-config", action="store_true", help=f"write an example config to {CONFIG_PATH}")
    ap.add_argument("--mcp", action="store_true", help="run as an MCP stdio server instead of an agent (for Claude Code / Desktop)")
    ap.add_argument("--no-advisory", action="store_true", help="omit the phyx3d printability/strength analysers")
    ap.add_argument("--model-dir", action="append", default=[], help="directory the analysers may read models from (repeatable; default: current dir)")
    ap.add_argument("--max-steps", type=int, default=40)
    args = ap.parse_args(argv)
    logging.basicConfig(level=os.environ.get("ORCA_AI_LOG", "WARNING"),
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")

    if args.init_config:
        print(f"Wrote {write_example()}; edit the model names, then export the API-key variables it names.")
        return 0
    if args.model_dir:
        os.environ["ORCA_AI_MODEL_DIRS"] = os.pathsep.join(args.model_dir)
    tools = load_tools(read_only=args.read_only, advisory=not args.no_advisory)
    if args.list_tools:
        for t in tools:
            flags = ("M" if t.mutating else "-") + ("D" if t.destructive else "-")
            print(f"{flags} {t.name:22} {t.method:6} {t.path}")
        return 0
    if args.mcp:
        from .mcp_server import serve
        serve(read_only=args.read_only, allow_destructive=args.yes)
        return 0
    if not args.request:
        ap.error("give a request, e.g. orca-ai 'review this model for strength; it is a bracket under load'")

    config = load_config()
    name = args.provider or config["default"]
    provider = build_provider(name, config["providers"][name])
    agent = Agent(provider, RemoteAPIClient(), tools, max_steps=args.max_steps,
                  confirm=(lambda *_: True) if args.yes else confirm_on_terminal)
    agent.run(" ".join(args.request))
    return 0


if __name__ == "__main__":
    sys.exit(main())
