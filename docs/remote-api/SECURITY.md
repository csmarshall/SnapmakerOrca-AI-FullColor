# Remote API: security stance and residual risks

Status: draft for issue #16. Written from the issue comments and the #53 design pass. "Verified" means read from source or exercised in a run; "projected" means reasoned about but not tested. Nothing here is a security audit, and the API has had no independent review.

The file is meant to live at `docs/remote-api/SECURITY.md` in the code repo (`ai-on-snapmaker`) and be linked from `docs/remote-api/README.md`. It is drafted in the plan repo because the code worktrees were busy; the copy and the README link are still to do.

## What the API is

A token-authenticated HTTP and WebSocket server inside the running slicer. Whoever holds the token can read and change the settings of the open project, load model files, slice, and (through the MCP tools) render. It is off by default and has to be switched on in Preferences.

## Defaults and the opt-ins

| Setting | Default | What opting in costs |
|---|---|---|
| Enable | off | The slicer now accepts commands from a local program. |
| Bind LAN | off, so `127.0.0.1` only | Listens on `0.0.0.0`. See below. |
| `remote_api_allow_sensitive` | off | Relaxes the sensitive-key checks for automation (exactly which keys is to be confirmed against the source before this document is final). Set only from a checkbox in Preferences; the API cannot grant it to itself (verified in source for the Remote API path). |
| `remote_api_model_dirs` | unset, so any local absolute path | When set, `loadModel` only accepts files under those folders. |

### Loopback (default)

Any process running as any user on the same machine can try to connect to a loopback port; the token is the only barrier. Treat the token like a password for the app: it is in the data folder (`.api-token`, mode 600 when created by `u25-launch.sh`) and in the `OrcaSlicer.conf` setting. Verified: a request with no or a wrong token gets `401`. Projected, not tested: that a local malicious process running as the same user cannot simply read the token file. It can, so loopback does not defend against malware already running as you.

### LAN binding (opt-in)

- Traffic is plain HTTP. There is no TLS. Anyone on the path can read the token from a request header, and can then use it. On a shared or untrusted network this is a full compromise of what the API can do.
- The token is a bearer credential with no expiry and no per-client identity. Regenerating it in Preferences invalidates all clients.
- There is no rate limiting or lockout that we have verified. A 32-hex-character token is not realistically guessable, but that is an estimate, not a test.
- The WebSocket takes the token as `?token=` in the URL, which tends to end up in logs and browser history.
- Recommendation if you need remote access: keep the bind on loopback and put an SSH tunnel or a TLS-terminating reverse proxy in front. Do not expose the port beyond a network you trust.

## Guards that exist (verified unless noted)

- Hard-blocked write keys: `post_process`, `printer_model`, `filename_format`, `gcode_substitutions`, `bed_custom_texture`, `bed_custom_model`, the print-host keys and every `*_gcode` option. A write to one is refused, even with the token. Checked by `tools/tests/live_api_check.py` against a running app.
- Reads of credential keys are redacted; writing the redacted placeholder back is refused.
- Work is refused with 409 while the GUI has unsaved edits, the editor is open, or a slice is running.
- The GUI work runs on the UI thread with a timeout, so a request cannot leave a modal waiting for a person.

## `loadModel` and the local-path stance

Decision (Charles, 2026-10-20): by default `loadModel` accepts any local absolute path. The allowlist `remote_api_model_dirs` stays opt-in.

Mitigations in place: UNC, NT-namespace and relative paths are refused; the file extension must be on an allowlist; when the allowlist is set it is checked before the file is probed; the canonical (symlink-resolved) path is what gets loaded and its extension is checked again.

Residual risk: a symlink, or an automount, that points at a network share is a local path, so it can be loaded. That can cause the slicer to reach out to a remote file server (projected, not tested: slow reads, credential prompts, or an authentication attempt to that server). With the default settings a token holder can also make the slicer open any readable local file that has an accepted extension.

Not tested: Windows path forms beyond the ones in `BAD_PATHS` in the live check. Treat the Windows behaviour as unverified.

Hardening to consider if the stance changes: default the allowlist to Documents, Downloads or a models folder, or reject non-local filesystems (`statfs` with `MNT_LOCAL`).

## Plugin and `post_process` write gap (#53, open)

Verified from source, read-only: the guard that refuses code-bearing keys covers the settings write path (`PresetTransaction::set`, used by the Remote API and the plugin host API). It does not cover a plugin that writes preset files directly:

- A plugin can write inside the data folder with ordinary file calls, including `user/<folder>/**` and `system/**`; the plugin audit hook protects only a few named files and path components.
- `reload_local` then loads such a bundle without looking at the values in it, so a `post_process` or `*_gcode` value written that way can become live in the same session.
- `/preset/save` saves whatever the edited preset holds, so a value that arrived that way can be copied into an ordinary user preset.
- The audit hook itself says it blocks direct access, not an actively evasive plugin, and `ctypes` or a subprocess can bypass any path rule.

Not exercised on a running app yet. Proposed layers (file-write protection, a content check on `reload_local`, a run-time approval for `post_process`) are in #53 and are not implemented. Until they are: install only plugins you trust, because an installed plugin can already do more than the Remote API's guards allow.

A 3MF can also carry `post_process` in its project settings. Not in the scope of this document or of #53.

## Analysers are advisory

The model and print analysers (and any "will this print" style verdicts surfaced through the MCP tools) are heuristics, and are modelled on Bambu printers. They can miss a problem or flag a good print. They are not a safety check; a person still has to look at the sliced preview before printing.

## Reporting

This is a private fork with no formal disclosure process yet. Open an issue in the plan repo.
