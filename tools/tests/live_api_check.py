#!/usr/bin/env python3
"""Live regression check for the Remote API guards, run against a launched slicer (not a fake).

    ORCA_API_TOKEN=... python3 live_api_check.py [--url http://127.0.0.1:13130]
    python3 live_api_check.py --selftest     # detector check: must FAIL against a server that accepts everything

It only sends requests that are either refused or idempotent (a value written back unchanged), so it is safe on a
scratch profile. Exit code 0 = every guard held, 1 = at least one check failed, 2 = could not run.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer

FORBIDDEN_WRITES = ["post_process", "printer_model", "filename_format", "gcode_substitutions", "bed_custom_texture",
                    "bed_custom_model", "layer_change_gcode", "machine_start_gcode", "printhost_apikey", "printhost_port",
                    "print_host", "print_host_webui", "bbl_use_printhost", "host_type"]
BAD_PATHS = ["//server/share/a.stl", "\\\\server\\share\\a.stl", "/\\server/a.stl", "\\/server/a.stl", "relative/a.stl",
             "\\??\\C:\\a.stl"]


class Api:
    def __init__(self, base: str, token: str):
        self.base, self.token = base.rstrip("/") + "/api/v1", token

    def call(self, method: str, path: str, body=None):
        req = urllib.request.Request(self.base + path, method=method, data=None if body is None else json.dumps(body).encode(),
                                     headers={"X-Api-Token": self.token, "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=30) as r:
                return r.status, r.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def j(self, method: str, path: str, body=None):
        status, text = self.call(method, path, body)
        try:
            return status, json.loads(text)
        except ValueError:
            return status, {"_raw": text[:200]}


def run_checks(api: Api) -> list[tuple[str, bool, str]]:
    out: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = ""):
        out.append((name, bool(ok), detail))

    status, st = api.j("GET", "/status")
    check("status reachable", status == 200 and "app" in st, f"HTTP {status}")

    for key in FORBIDDEN_WRITES:
        status, r = api.j("PUT", "/config", {key: "1"})
        check(f"forbidden write refused: {key}", r.get("applied") == [] and r.get("errors", {}).get(key) == "forbidden_key",
              f"HTTP {status} {json.dumps(r)[:100]}")

    # control: an idempotent write must go through, so a refuse-everything bug cannot pass the checks above
    _, cur = api.j("GET", "/config?keys=layer_height")
    value = cur.get("config", {}).get("layer_height")
    status, r = api.j("PUT", "/config", {"layer_height": value})
    check("control: allowed key is accepted", "layer_height" in r.get("applied", []), f"HTTP {status} {json.dumps(r)[:100]}")

    printer = st.get("presets", {}).get("printer", "")
    status, r = api.j("POST", "/preset/config", {"type": "printer", "name": printer})
    cfg = r.get("config", {})
    hosts = {k: v for k, v in cfg.items() if k.startswith(("print_host", "printhost_")) or k in ("bbl_use_printhost", "host_type")}
    check("preset read redacts every print-host value", bool(hosts) and all(v == "[redacted]" for v in hosts.values()),
          f"{len(hosts)} keys, unredacted: {[k for k, v in hosts.items() if v != '[redacted]'][:4]}")

    for path in BAD_PATHS:
        status, r = api.j("POST", "/model", {"path": path})
        check(f"loadModel refuses {path!r}", r.get("error") == "path_not_allowed", f"HTTP {status} {r.get('error')}")
    status, r = api.j("POST", "/model", {"path": "/tmp/x.txt"})
    check("loadModel refuses a non-model extension", r.get("error") == "unsupported_format", f"HTTP {status} {r.get('error')}")
    status, r = api.j("POST", "/model", {"path": "/tmp/orca-live-check-does-not-exist.stl"})
    check("loadModel reports a missing file", r.get("error") == "not_found", f"HTTP {status} {r.get('error')}")

    slots = st.get("presets", {}).get("filaments", [])
    check("status lists filament slots", isinstance(slots, list) and len(slots) >= 1, str(slots))
    if slots:
        status, r = api.j("PUT", "/preset", {"type": "filament", "name": slots[0], "extruder": 0})
        _, st2 = api.j("GET", "/status")
        check("control: re-selecting a slot's own filament succeeds and changes nothing",
              status == 200 and r.get("extruder") == 0 and st2.get("presets", {}).get("filaments") == slots, f"HTTP {status} {r}")
        status, r = api.j("PUT", "/preset", {"type": "filament", "name": slots[0], "extruder": 99})
        check("filament slot out of range refused", status == 422 and r.get("error") == "bad_extruder", f"HTTP {status} {r}")
        status, r = api.j("PUT", "/preset", {"type": "filament", "name": slots[0], "extruder": -1})
        check("negative filament slot refused", status == 400 and r.get("error") == "bad_extruder", f"HTTP {status} {r}")
        status, r = api.j("PUT", "/preset", {"type": "print", "name": st["presets"]["print"], "extruder": 1})
        check("extruder on a non-filament type refused", status == 400 and r.get("error") == "bad_extruder", f"HTTP {status} {r}")
    status, r = api.j("PUT", "/preset", {"type": "filament", "name": "No Such Filament Preset"})
    check("unknown preset refused", status == 422 and r.get("error") == "unknown_preset", f"HTTP {status} {r}")

    status, r = api.j("GET", "/presets?type=bogus")
    check("presets filter rejects an unknown type", status == 400 and r.get("error") == "bad_param", f"HTTP {status} {r}")
    status, r = api.j("GET", "/presets?type=printer")
    check("presets filter returns only the requested collection", set(r) == {"printer"}, f"keys {sorted(r)}")

    status, _ = api.call("GET", "/status")
    bad = Api(api.base.rsplit("/api/v1", 1)[0], "not-the-token")
    status_bad, _ = bad.call("GET", "/status")
    check("wrong token is refused", status_bad in (401, 403), f"HTTP {status_bad}")
    return out


class _AcceptEverything(BaseHTTPRequestHandler):
    """A broken 'slicer' that applies every write and accepts every token: the checks above must fail against it."""
    def _send(self, status: int, body: dict):
        data = json.dumps(body).encode()
        self.send_response(status); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(data)))
        self.end_headers(); self.wfile.write(data)

    def do_GET(self):
        if self.path.startswith("/api/v1/status"):
            return self._send(200, {"app": "fake", "presets": {"printer": "P", "print": "Q", "filaments": ["F1", "F2"]}})
        if "layer_height" in self.path:
            return self._send(200, {"config": {"layer_height": "0.2"}})
        self._send(200, {"printer": [], "filament": [], "print": []})

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def do_PUT(self):
        body = self._body()
        self._send(200, {"applied": list(body), "errors": {}, "selected": body.get("name"), "extruder": body.get("extruder", 0)})

    def do_POST(self):
        body = self._body()
        if self.path.endswith("/preset/config"):
            return self._send(200, {"config": {"printhost_apikey": "SECRET", "print_host": "http://u:p@h"}})
        self._send(200, {"loaded": True, "path": body.get("path")})

    def log_message(self, *_):
        pass


def selftest() -> int:
    server = HTTPServer(("127.0.0.1", 0), _AcceptEverything)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    results = run_checks(Api(f"http://127.0.0.1:{server.server_port}", "t"))
    failed = [n for n, ok, _ in results if not ok]
    server.shutdown()
    if len(failed) >= len(results) // 2:
        print(f"selftest OK: the checker failed {len(failed)} of {len(results)} checks against an accept-everything server")
        return 0
    print(f"selftest FAILED: only {len(failed)} of {len(results)} checks failed against an accept-everything server; the checker cannot detect broken guards")
    return 1


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default=os.environ.get("ORCA_API_URL", "http://127.0.0.1:13130"))
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    token = os.environ.get("ORCA_API_TOKEN")
    if not token:
        print("set ORCA_API_TOKEN", file=sys.stderr)
        return 2
    try:
        results = run_checks(Api(args.url, token))
    except OSError as exc:
        print(f"cannot reach {args.url}: {exc}", file=sys.stderr)
        return 2
    for name, ok, detail in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + ("" if ok else f"    [{detail}]"))
    failed = [n for n, ok, _ in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
