# Snapmaker Flutter web bundle (`resources/web/flutter_web`)

## Origin

- Component: the compiled Flutter web build that Snapmaker Orca uses for its Home and Device pages.
- Taken from: Snapmaker/OrcaSlicer `resources/web/flutter_web`, source commit `a00dd4fbac`.
- Version: `2.3.40`, build `20260930162817` (see `resources/web/flutter_web/version.json`).
- Vendored byte-identical. No file in the directory has been edited. `resources/web/flutter_web.sha256` lists the SHA-256 of every file, and `scripts/verify_flutter_web.cmake` (run as the ctest test `flutter_web_bundle_manifest`) fails on any changed, missing or extra file, an empty manifest or an empty bundle. The manifest is a test-time consistency check only: it is not a runtime tamper control, and nothing verifies the files when the app runs.
- Only the compiled output is included. **No Dart source is included** in this repository.

## Licence and source obligations: UNRESOLVED

The bundle is shipped as a separate, attributed component (ADR 0005, decision 3). Snapmaker Orca is AGPL-3.0, and whether and how the AGPL source obligations apply to a compiled bundle whose Dart source is not available to us is **unresolved**. Charles accepted that risk for now; the open question is whether to ask Snapmaker for the source or a licence statement. Revisit before any public release (#56).

## Privacy disclosure

When the page loads it contacts these hosts:

- `api.snapmaker.com`
- `id.snapmaker.com`
- `public.resource.snapmaker.com`
- `fonts.gstatic.com`

It also ships a PostHog analytics script (`posthog-array.*.js`) and an `sw_UploadEvent` telemetry command. We do not block or strip any of this (ADR 0005, decision 6: disclose only). Opening the Snapmaker page reveals the user's IP address to those hosts. This text must also appear in the README and the About page.

## How it is served

Only through the hardened loopback page server (`src/slic3r/GUI/WebPageServer.*`), never fetched at runtime and never opened as a `file://` URL.

## Content-Security-Policy

`WebPageServer` sends the policy in `resources/web/flutter_web.csp` as the `Content-Security-Policy` header on **every** response (200, 302, 403, 404, 405, HEAD), not only HTML. The server reads the file when it starts and **refuses to start** if it is missing, empty, over 8 KiB, contains control characters or contains `'unsafe-inline'` / `'unsafe-eval'` (fail closed: it never serves without a policy).

The policy is "policy F" of experiment 7g (`docs/snapmaker-page-contract.md` in the plan repo), verified in headless Chrome with zero violations on the Home and Device routes, with one deliberate change: `https://us.i.posthog.com` is added to `connect-src`, so the page's analytics is not blocked (ADR 0005 point 6, disclose only). Removing that one token blocks it. It sets `default-src 'none'`, `frame-src 'none'`, `object-src 'none'`, `base-uri 'self'` and `form-action 'none'`; scripts are `'self'` plus `'wasm-unsafe-eval'` (CanvasKit needs WebAssembly) plus one hash for the single inline script; styles use five hashes and no `'unsafe-inline'`. Hosts allowed beyond `'self'`: `id.snapmaker.com`, `api.snapmaker.com`, `public.resource.snapmaker.com`, `fonts.gstatic.com`, `us.i.posthog.com`. Anything else the page tries to load is blocked, which also narrows the privacy disclosure above to exactly these hosts.

### The hashes belong to bundle 2.3.40

`scripts/verify_csp_hashes.cmake` (ctest `snapmaker_csp_covers_inline_blocks`) recomputes the SHA-256 of the static inline `<script>` and `<style>` blocks in `index.html` and fails if one is missing from `script-src` / `style-src-elem`, if `script-src` carries a hash no inline script has, or if `index.html` gains an inline `style=`/`on*=` attribute. It cannot recompute the others: three `style-src-elem` hashes (runtime-created `<style>` elements, one inside a shadow root), `style-src-attr` (the overlay's parsed `style="display:none;"`) and `script-src-attr` (the overlay's Reload `onclick`). Those were verified in headless Chrome for 2.3.40 only, and the script prints a NOTE saying so.

### Bundle bump procedure

1. Replace the bundle, regenerate `flutter_web.sha256`, update `version.json` notes above.
2. `ctest -R snapmaker_csp` and `ctest -R snapmaker_bridge_commands` (new `sw_*` names must be classified in `resources/web/snapmaker_bridge_commands.txt`).
3. Run `python3 -I scripts/csp_report_only_check.py` (needs Chrome or Chromium; `--chrome PATH` otherwise). It serves the bundle from a local 127.0.0.1 server with the policy as `Content-Security-Policy-Report-Only`, loads the Home (`?path=1`) and Device (`?path=2`) routes headless with external hosts blocked at the resolver, and fails on any Content Security Policy console message. It only sees what the page does before it gets a host bridge, so also drive the pages with a stub bridge (the 7g harness idea: answer the page's `window.wx.postMessage` commands from a recorded file and collect `securitypolicyviolation` events over CDP) for the routes you care about.
4. For each violation, recompute the hash Chrome prints (it names the `sha256-...` it expected) or widen the narrowest directive; never add `'unsafe-inline'`/`'unsafe-eval'` (the server refuses such a file).

### Not covered / printer origins

The timelapse, print and send pages were never driven and stay denied at the bridge. The **camera preview** (#76) is wired:
- `SnapmakerDevicePage` takes the selected machine (`get_dev_ip()`, else the dev id), turns it into `http://<bare host>` with `snapmaker_camera_origin_for()` and hands it to `WebPageServer::set_extra_origin()` before `start()`. A warning is logged when there is no usable origin (no machine selected, or an address that is not a plain host, e.g. an unbracketed IPv6 dev id); the camera tile is then blocked by the CSP.
- `start()` serves `web_page_server_detail::csp_with_extra_origin(policy, origin)`, which appends the origin to `img-src` and `connect-src` and to **nothing else** (no `media-src` is added or created: the page loads a JPEG through img/fetch). `set_extra_origin` accepts only `http://` plus a DNS name, IPv4 address or bracketed IPv6 literal (no credentials, port, path, spaces or semicolons); anything else is refused and the static `flutter_web.csp` stays in force. That file is unchanged and still fails closed.
- When the Device page is shown again and the selected machine's origin differs from the one the page was started with (including one appearing after the page started with none), `Show(true)` calls `reset()` and `start()`, so the page and its CSP match. Decision: `snapmaker_camera_origin_needs_reload()`.
- The camera RPC (`MoonrakerPrinterAgent::camera_monitor_async`) goes to the agent's own `device_info.base_url`, while the CSP origin comes from DeviceManager's selected machine. Both name the connected machine in practice but are two sources; if they ever differ the tile is blocked rather than leaking anywhere. They were not unified.
- At most one camera RPC is in flight per agent; a further call is refused at once (`done(false)`, no thread, no socket), not queued. The whole RPC (resolve, connect, handshake, reply) is bounded by 5 s, the connect time shared across the resolved addresses.

Known limitations (not solved on purpose):
- The origin is fixed per page load; it is refreshed only when the page is shown again (above), not while it stays visible.
- IPv6 machines: `parse_ws_endpoint` does not handle bracketed hosts, so the camera RPC cannot reach an IPv6 machine, and the origin is only served for a bracketed literal (unverified in WKWebView).
- The page's `sw_CameraStopMonitor` is the only stop: nothing stops the monitor when the page or Orca is torn down.
- Only plain `ws://` machines work; an `https://` machine fails the camera RPC.

WKWebView and Edge WebView2 CSP behaviour, including hash matching of inline content, has only been checked against Chromium so far.

## CSP: WebKit (macOS wxWebView) differs from Chrome (2026-11-06)

Policy F was verified in headless Chrome only, and the real macOS WebView (WKWebView) showed the bundle's own "Load failed" overlay with it. Cause, read from `securitypolicyviolation` events inside the real WebView: the Flutter engine builds its `<style>` from text pieces that differ per browser engine, so the WebKit variant (1063 chars, `sha256-CCNlQB4n2yV+kpblVxCg1CPdS/azQfc966N89UgaYs4=`) was never hashed; WebKit raises an `error` event on the blocked `<style>`, and `flutter_bootstrap` treats any such event before the first frame as fatal. `img-src` also needed `blob:` (images the page decodes itself). Both are now in `resources/web/flutter_web.csp`.

On every bundle bump, or on a new platform's WebView (Windows WebView2 is expected to match Chrome but is unverified), re-check by loading the page in that WebView and watching for `securitypolicyviolation` / "Load failed". The static-hash ctest gate cannot catch engine-generated styles. One violation is still reported on macOS (an inline style, empty sample) that does not stop the page from rendering; not yet explained.
