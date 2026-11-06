#!/usr/bin/env python3
"""Re-verifies resources/web/flutter_web.csp against the vendored Snapmaker bundle (run after every bundle bump).

Serves resources/web/flutter_web on 127.0.0.1 with the policy sent as Content-Security-Policy-Report-Only, loads the Home route
(?path=1) and the Device route (?path=2) in headless Chrome, and fails if Chrome logs ANY Content Security Policy violation.
External hosts are blocked at the resolver (--host-resolver-rules), so nothing leaves the machine; a blocked request is a network
error, not a CSP violation, so the check still sees every violation that happens before the request.

Optional, not a ctest (needs Chrome). Limits: there is no host bridge (window.wx), so the page stops after its first commands; the
runtime-created styles that were hashed (overlay theme, flt-scene-host, shadow-root <style>) appear only once Flutter has started.
It does not drive Add Device, print/send, camera or timelapse. Treat a pass as "no violation on first render", and add a drive
(see docs/third-party/flutter_web.md) before trusting a new bundle.

Usage: python3 -I scripts/csp_report_only_check.py [--chrome PATH] [--seconds N]
"""
import argparse, http.server, os, subprocess, sys, threading, shutil

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RES = os.path.join(ROOT, "resources")
WEB = os.path.join(RES, "web")
CSP_FILE = os.path.join(WEB, "flutter_web.csp")
CHROMES = ["/Applications/Google Chrome.app/Contents/MacOS/Google Chrome", "google-chrome", "chromium", "chromium-browser"]


def read_policy(path=CSP_FILE):
    lines = [l.strip() for l in open(path, encoding="utf-8") if l.strip() and not l.lstrip().startswith("#")]
    if not lines:
        sys.exit("empty policy file: " + path)
    return " ".join(lines)


def make_handler(policy):
    class H(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=RES, **k)

        def end_headers(self):
            self.send_header("Content-Security-Policy-Report-Only", policy)
            self.send_header("Cache-Control", "no-store")
            super().end_headers()

        def log_message(self, *a):
            pass

    return H


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chrome")
    ap.add_argument("--csp", default=CSP_FILE, help="policy file to test (default: resources/web/flutter_web.csp)")
    ap.add_argument("--seconds", type=int, default=12)
    a = ap.parse_args()
    chrome = a.chrome or next((c for c in CHROMES if os.path.exists(c) or shutil.which(c)), None)
    if not chrome:
        sys.exit("Chrome/Chromium not found; pass --chrome")
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), make_handler(read_policy(a.csp)))
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    bad = 0
    for route in ("1", "2"):
        # The bundle's <base href> is /web/flutter_web/, so serve resources/ and keep that path.
        url = "http://127.0.0.1:%d/web/flutter_web/index.html?path=%s" % (port, route)
        cmd = [chrome, "--headless=new", "--disable-gpu", "--no-first-run", "--user-data-dir=" + os.path.join(ROOT, "build", ".csp_check_profile"),
               "--host-resolver-rules=MAP * ~NOTFOUND , EXCLUDE 127.0.0.1", "--enable-logging=stderr", "--v=0",
               "--virtual-time-budget=%d" % (a.seconds * 1000), "--dump-dom", url]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=a.seconds * 4 + 30)
        hits = [l for l in r.stderr.splitlines() if "Content Security Policy" in l or "Content-Security-Policy" in l]
        print("route path=%s: %d violation(s)" % (route, len(hits)))
        for l in hits[:20]:
            print("   ", l[:300])
        bad += len(hits)
    srv.shutdown()
    if bad:
        sys.exit("FAIL: %d CSP violation(s); the policy needs updating for this bundle" % bad)
    print("OK: no CSP violation on first render of Home and Device")


if __name__ == "__main__":
    main()
