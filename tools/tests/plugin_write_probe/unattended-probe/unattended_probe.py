# /// script
# requires-python = ">=3.12"
#
# [tool.orcaslicer.plugin]
# id = "unattended-probe"
# name = "Unattended Probe"
# description = "Test probe for #51: tries a blocked and an allowed preset write once the app is up."
# author = "scratch"
# version = "0.1.0"
# type = "script"
# ///
import os
import threading
import time
import traceback

import orca

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "probe_result.txt")


def _try(key, value):
    try:
        orca.host.preset.process.update(key, value)
        return f"{key}: APPLIED"
    except BaseException as exc:
        return f"{key}: RAISED {type(exc).__name__}: {exc}"


def _worker():
    time.sleep(25)
    lines = []
    try:
        lines.append(_try("layer_height", "0.16"))          # control: allowed key must apply
        lines.append(_try("post_process", "echo probe51"))  # must be refused on the fixed build
        lines.append(_try("printhost_apikey", "x"))         # not a process key; either refusal is fine
    except BaseException:
        lines.append("WORKER-ERROR " + traceback.format_exc())
    with open(OUT, "w") as f:
        f.write("\n".join(lines) + "\n")


class UnattendedProbe(orca.script.ScriptPluginCapabilityBase):
    def get_name(self):
        return "unattended-probe"

    def on_load(self):
        threading.Thread(target=_worker, daemon=True).start()

    def execute(self):
        return orca.ExecutionResult.success("probe")


@orca.plugin
class UnattendedProbePackage(orca.base):
    def register_capabilities(self):
        orca.register_capability(UnattendedProbe)
