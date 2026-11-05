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
