# orca-ai

Natural-language control of a running OrcaSlicer (FullSpectrum build) through its Remote API. Provider-pluggable: Claude, OpenAI, or any OpenAI-compatible server (Ollama / vLLM / llama.cpp on a LAN box). Every tool is generated from `docs/remote-api/openapi.yaml`, so adding an endpoint to the spec adds it to every provider and to MCP with no other change.

## Setup

1. In OrcaSlicer: Preferences > Remote API > Enable; copy the token. `export ORCA_API_TOKEN=...` (and `ORCA_API_URL` if not `http://127.0.0.1:13130`).
2. `python3 -m orca_ai --init-config` writes `~/.config/orca-ai/config.json`. Edit model names. Keys are read from the environment variables the config names, never stored.
3. Run from `tools/`: `python3 -m orca_ai "review this model; it is a bracket that carries 5 kg, make it stronger"`.

Needs `requests` and `pyyaml`. Pin the interpreter in unattended scripts (see the toad `python3` note in CLAUDE.md).

## Use from Claude Code / Claude Desktop (MCP)

```json
{"mcpServers": {"orca": {"command": "python3", "args": ["-m", "orca_ai", "--mcp"],
  "cwd": "<repo>/tools", "env": {"ORCA_API_TOKEN": "..."}}}}
```

## Safety

- `--read-only` exposes only tools that change nothing. Destructive tools (`deletePreset`) are hidden from MCP unless `--yes`, and require terminal confirmation in agent mode.
- Edits go through the GUI's own edit path, so they appear as modified presets and can be undone by discarding changes. Slicing and rendering are the only checks; nothing here proves a part is strong.

## Tests

`python3 -m unittest discover -s tests`. They run against a fake Remote API and a scripted provider, so they show the requests match the spec and the loop and guardrails behave. They do not show the real Orca server accepts those requests; that needs a live run.
