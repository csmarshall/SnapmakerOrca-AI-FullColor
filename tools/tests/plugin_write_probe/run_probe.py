"""Run one Orca build with a copy of the test data dir plus the #51 probe plugin; print the probe result.

usage: run_probe.py <app binary> <label> [source data dir, default ~/orca25-test] [--expect applied|raised] [--keep]

Copies a configured data dir (a fresh one opens the setup wizard) into a fresh temp directory outside any
repo, installs the probe pre-enabled with its thread permission granted, launches the app, waits for
probe_result.txt, then stops the app and deletes the copy (--keep leaves it and prints its path).
Expected: stock 2.5.0-beta APPLIES post_process; a build with the plugin deny list RAISES; layer_height APPLIES on both.
Exit: 0 = post_process matched --expect and layer_height APPLIED; 1 = mismatch; 2 = no result / app exited early / bad usage.
"""
import argparse, os, re, shutil, signal, subprocess, sys, tempfile, time

LABEL_RE = re.compile(r"^[A-Za-z0-9._-]+$")


def parse_result(text, key):
    """'APPLIED' / 'RAISED' for `key` in the probe's output, or None when the line is missing."""
    m = re.search(rf"^{re.escape(key)}: (APPLIED|RAISED)\b", text, re.M)
    return m.group(1).lower() if m else None


def verdict(text, expect):
    """0 when layer_height applied and post_process matched `expect` (None = no expectation), else 1."""
    layer, post = parse_result(text, "layer_height"), parse_result(text, "post_process")
    ok = layer == "applied" and (expect is None or post == expect)
    print(f"verdict: layer_height={layer} post_process={post} expected post_process={expect}: {'OK' if ok else 'MISMATCH'}")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("binary")
    ap.add_argument("label")
    ap.add_argument("source", nargs="?", default=os.path.expanduser("~/orca25-test"))
    ap.add_argument("--expect", choices=["applied", "raised"], help="what post_process must do for exit 0")
    ap.add_argument("--keep", action="store_true", help="keep the copied data dir and print its path")
    args = ap.parse_args()
    if not LABEL_RE.match(args.label):
        print("label must match ^[A-Za-z0-9._-]+$", file=sys.stderr)
        return 2
    here = os.path.dirname(os.path.abspath(__file__))
    if subprocess.run(["pgrep", "-f", "OrcaSlicer.app/Contents/MacOS/OrcaSlicer|Snapmaker_Orca"], capture_output=True).returncode == 0:
        print("another Orca is running; stop it first", file=sys.stderr)
        return 2
    dd = tempfile.mkdtemp(prefix=f"orca-probe-{args.label}-")
    p = None
    code = 2
    try:
        shutil.copytree(args.source, dd, dirs_exist_ok=True, ignore=shutil.ignore_patterns("app.log", "log", "cache", "orca_plugins"))
        plug = os.path.join(dd, "orca_plugins", "unattended-probe")
        shutil.copytree(os.path.join(here, "unattended-probe"), plug)
        out = os.path.join(plug, "probe_result.txt")
        print("launching", args.binary, "--datadir", dd, flush=True)
        with open(os.path.join(dd, "app.log"), "w") as log:
            p = subprocess.Popen([args.binary, "--datadir", dd], stdout=log, stderr=subprocess.STDOUT)
            for _ in range(180):
                if os.path.exists(out) and os.path.getsize(out) > 0:
                    time.sleep(1)
                    break
                if p.poll() is not None:
                    print("app exited early, code", p.returncode)
                    break
                time.sleep(1)
        if os.path.exists(out) and os.path.getsize(out) > 0:
            text = open(out).read()
            print(text)
            code = verdict(text, args.expect)
        else:
            print("NO RESULT FILE (probe never ran)")
        side = os.path.join(plug, ".install_state.json")
        print("sidecar enabled after run:", '"enabled": true' in open(side).read() if os.path.exists(side) else "missing")
    finally:
        if p is not None and p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try:
                p.wait(30)
            except subprocess.TimeoutExpired:
                p.kill()
        print("stopped")
        if args.keep:
            print("kept data dir:", dd)
        else:
            shutil.rmtree(dd, ignore_errors=True)
    return code


if __name__ == "__main__":
    sys.exit(main())
