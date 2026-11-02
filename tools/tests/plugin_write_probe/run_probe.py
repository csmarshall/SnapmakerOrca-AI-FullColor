"""Run one Orca build with a copy of the test data dir plus the #51 probe plugin; print the probe result.
usage: run_probe.py <app binary> <label> [source data dir, default ~/orca25-test]
Copies a configured data dir (a fresh one opens the setup wizard), installs the probe pre-enabled with
its thread permission granted, launches the app, waits for probe_result.txt, then stops the app.
Expected: stock 2.5.0-beta APPLIES post_process; a build with the plugin deny list RAISES; layer_height APPLIES on both."""
import os, shutil, signal, subprocess, sys, time
binary, label = sys.argv[1], sys.argv[2]
here = os.path.dirname(os.path.abspath(__file__))
dd = os.path.join(here, f"e2e-{label}")
if subprocess.run(["pgrep", "-f", "OrcaSlicer.app/Contents/MacOS/OrcaSlicer|Snapmaker_Orca"], capture_output=True).returncode == 0:
    sys.exit("another Orca is running; stop it first")
shutil.rmtree(dd, ignore_errors=True)
shutil.copytree(sys.argv[3] if len(sys.argv) > 3 else os.path.expanduser("~/orca25-test"), dd, ignore=shutil.ignore_patterns("app.log", "log", "cache", "orca_plugins"))
plug = os.path.join(dd, "orca_plugins", "unattended-probe")
shutil.copytree(os.path.join(here, "unattended-probe"), plug)
out = os.path.join(plug, "probe_result.txt")
print("launching", binary, "--datadir", dd, flush=True)
log = open(os.path.join(dd, "app.log"), "w")
p = subprocess.Popen([binary, "--datadir", dd], stdout=log, stderr=subprocess.STDOUT)
try:
    for _ in range(180):
        if os.path.exists(out) and os.path.getsize(out) > 0:
            time.sleep(1); break
        if p.poll() is not None:
            print("app exited early, code", p.returncode); break
        time.sleep(1)
    print(open(out).read() if os.path.exists(out) else "NO RESULT FILE (probe never ran)")
    side = os.path.join(plug, ".install_state.json")
    print("sidecar enabled after run:", '"enabled": true' in open(side).read() if os.path.exists(side) else "missing")
finally:
    if p.poll() is None:
        p.send_signal(signal.SIGTERM)
        try: p.wait(30)
        except subprocess.TimeoutExpired: p.kill()
    print("stopped")
