#!/usr/bin/env python3
"""Live regression check for the Remote API guards, run against a launched slicer (not a fake).

    ORCA_API_TOKEN=... python3 live_api_check.py [--url http://127.0.0.1:13130]
    python3 live_api_check.py --selftest     # detector check: must pass only the controls against a server that accepts everything, and fail only them against one that refuses everything

It only sends requests that are either refused or idempotent (a value written back unchanged), so it is safe on a
scratch profile. Exit code 0 = every guard held, 1 = at least one check failed, 2 = could not run (unreachable, no token, or an unexpected response shape).
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
                    "print_host", "print_host_webui", "bbl_use_printhost", "host_type",
                    "imex_mode_gcodes", "load_custom_gcodes", "filament_start_gcode"]
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


def _slot_runs(api: Api, key: str, n_slots: int) -> list[list[str]] | None:
    """Split GET /config's merged per-filament value into one run per slot. Runs are NOT equal
    width: each slot contributes one value per extruder variant of its preset, and
    filament_self_index (1-based slot per merged position) says which position is whose."""
    _, cur = api.j("GET", f"/config?keys={key},filament_self_index")
    cfg = cur.get("config", {})
    vals = str(cfg.get(key, "")).split(",")
    owner = [int(x) for x in str(cfg.get("filament_self_index", "")).split(",") if x.strip()]
    if len(owner) != len(vals):
        return None
    runs: list[list[str]] = [[] for _ in range(n_slots)]
    for v, o in zip(vals, owner):
        if not 1 <= o <= n_slots:
            return None
        runs[o - 1].append(v)
    return runs


def _filament_slot_write(api: Api, slots: list[str]) -> list[tuple[str, bool, str]]:
    """Write nozzle_temperature to the LAST slot via filament_slot: exactly the slots sharing its
    preset must change, the rest must not; then write the old value back."""
    key, target = "nozzle_temperature", len(slots) - 1
    before = _slot_runs(api, key, len(slots))
    if before is None:
        return [("filament_slot write changes only the slots sharing that preset", False, "merged value does not split per slot")]
    sharing = [i for i, s in enumerate(slots) if s == slots[target]]
    # The write takes the preset's own vector (one value per extruder variant), not the merged run.
    _, pc = api.j("POST", "/preset/config", {"type": "filament", "name": slots[target]})
    old = str(pc.get("config", {}).get(key, ""))
    new = ",".join(str(int(float(v)) + 1) for v in old.split(","))
    rs, rr = None, {}
    try:
        status, r = api.j("PUT", "/config", {"filament_slot": target, key: new})
        after = _slot_runs(api, key, len(slots)) or []
    finally:  # always put the previous value back, even if the write or the read-back blew up
        rs, rr = api.j("PUT", "/config", {"filament_slot": target, key: old})
    changed = [i for i in range(len(slots)) if after and after[i] != before[i]]
    restored = _slot_runs(api, key, len(slots)) == before
    info = r.get("filament_slot", {})
    return [
        ("filament_slot write changes only the slots sharing that preset",
         status == 200 and changed == sharing and info.get("slots_sharing_preset") == sharing and info.get("preset") == slots[target],
         f"HTTP {status} changed {changed} expected {sharing} info {info}"),
        ("filament_slot write restored", rs == 200 and restored, f"HTTP {rs} {json.dumps(rr)[:100]}"),
    ]


def _applied_keys(r: dict) -> list[str]:
    applied = r.get("applied", []) if isinstance(r, dict) else []
    return [str(k) for k in applied] if isinstance(applied, list) else []


def guarded_write(api: Api, body: dict) -> tuple[int, dict]:
    """PUT /config for a write that SHOULD be refused. If the slicer applies it anyway, put the previous
    values back (read first) so a broken guard does not leave the scratch profile modified."""
    keys = [k for k in body if k != "filament_slot"]
    _, cur = api.j("GET", "/config?keys=" + ",".join(keys))
    previous = {k: v for k, v in (cur.get("config", {}) if isinstance(cur, dict) else {}).items() if k in keys}
    status, r = None, {}
    try:
        status, r = api.j("PUT", "/config", body)
    finally:
        leaked = [k for k in _applied_keys(r) if k in previous]
        if leaked:
            api.j("PUT", "/config", {k: previous[k] for k in leaked})
    return status, r


def restore_selection(api: Api, st: dict) -> None:
    """Re-select the print preset and every filament slot recorded in `st` (a GET /status body)."""
    presets = st.get("presets", {})
    if presets.get("print"):
        api.j("PUT", "/preset", {"type": "print", "name": presets["print"]})
    for i, name in enumerate(presets.get("filaments", [])):
        api.j("PUT", "/preset", {"type": "filament", "name": name, "extruder": i})


def _volumes(api: Api, oid) -> list[dict]:
    status, r = api.j("GET", f"/objects/{oid}")
    vols = r.get("volumes") if status == 200 and isinstance(r, dict) else None
    return vols if isinstance(vols, list) else []


def _is_host_key(k: str) -> bool:
    return k.startswith(("print_host", "printhost_")) or k in ("bbl_use_printhost", "host_type")


def _object_checks(api: Api, check) -> None:
    """Part-level settings and modifier volumes. Needs one object on the plate; every write restores in `finally`."""
    status, r = api.j("GET", "/objects")
    objects = r.get("objects", []) if status == 200 and isinstance(r, dict) else []
    check("fixture: an object is loaded", len(objects) >= 1, f"HTTP {status} count {len(objects)}")
    if not objects:
        return
    oid = objects[0]["id"]
    before = _volumes(api, oid)
    parts = [v for v in before if v.get("type") == "part"]
    check("object lists its volumes", bool(parts), f"{len(before)} volumes")
    base = f"/objects/{oid}/volumes"
    part = parts[0].get("index", 0) if parts else 0

    def volume_write(path: str, body: dict) -> tuple[int, dict]:
        prev = {k: v for k, v in (parts[0].get("config") or {}).items() if k in body} if parts else {}
        status, r = None, {}
        try:
            status, r = api.j("PUT", path, body)
        finally:
            leaked = [k for k in _applied_keys(r) if k in prev]
            if leaked:
                api.j("PUT", path, {k: prev[k] for k in leaked})
        return status, r

    status, r = volume_write(f"{base}/{part}/config", {"post_process": "x"})
    check("volume forbidden write refused", r.get("applied") == [] and r.get("errors", {}).get("post_process") == "forbidden_key",
          f"HTTP {status} {json.dumps(r)[:120]}")
    status, r = volume_write(f"{base}/{part}/config", {"printer_agent": "live-check-not-an-agent"})
    check("volume policy-gated change refused", r.get("applied") == [] and r.get("errors", {}).get("printer_agent") == "blocked_by_remote_api_policy",
          f"HTTP {status} {json.dumps(r)[:120]}")
    status, r = api.j("PUT", f"{base}/9999/config", {"wall_loops": 2})
    check("unknown volume is 404", status == 404 and r.get("error") == "unknown_volume", f"HTTP {status} {r}")
    status, r = api.j("PUT", f"{base}/x/config", {"wall_loops": 2})
    check("bad volume index is 400", status == 400 and r.get("error") == "bad_volume_index", f"HTTP {status} {r}")

    for label, size in (("zero", [0, 5, 5]), ("huge", [1e9, 5, 5])):
        status, r = api.j("POST", f"/objects/{oid}/modifiers", {"shape": "box", "size": size})
        check(f"modifier with a {label} size refused", status == 400 and r.get("error") == "bad_modifier", f"HTTP {status} {r}")
        if status == 200:   # the guard did not hold: take the stray volume away again
            for v in _volumes(api, oid):
                if v.get("id") == (r.get("volume") or {}).get("id"):
                    api.j("DELETE", f"{base}/{v.get('index')}")
    for label, shape in (("number", 5), ("null", None), ("list", []), ("object", {})):
        status, r = api.j("POST", f"/objects/{oid}/modifiers", {"shape": shape, "size": [1, 1, 1]})
        check(f"modifier with a {label} shape is 400 bad_modifier", status == 400 and r.get("error") == "bad_modifier", f"HTTP {status} {json.dumps(r)[:120]}")
        if status == 200:
            for v in _volumes(api, oid)[len(before):]:
                api.j("DELETE", f"{base}/{v.get('index')}")
    status, r = api.j("GET", f"/objects/{oid}")
    held = [r.get("config") or {}] + [(v.get("config") or {}) for v in (r.get("volumes") or [])] if status == 200 and isinstance(r, dict) else [{}]
    leaked = sorted({k for c in held for k, v in c.items() if _is_host_key(k) and v != "<redacted>"})
    check("object and volume reads redact print-host keys", status == 200 and not leaked, f"HTTP {status} unredacted {leaked[:4]}")
    status, r = api.j("POST", f"/objects/{oid}/modifiers", {"shape": "box", "size": [5, 5, 5], "config": {"post_process": "x"}})
    after = _volumes(api, oid)
    check("modifier with a forbidden initial key refused and nothing added", status == 422 and len(after) == len(before),
          f"HTTP {status} volumes {len(before)} -> {len(after)}")
    if len(after) > len(before):
        for v in after[len(before):]:
            api.j("DELETE", f"{base}/{v.get('index')}")
    status, r = api.j("DELETE", f"{base}/{part}")
    check("deleting a part volume refused", status == 422 and r.get("error") == "cannot_delete_part", f"HTTP {status} {r}")

    # Control: add a modifier, see it listed with its override, write the value back, delete it.
    ok, detail, vid = False, "", None
    try:
        status, r = api.j("POST", f"/objects/{oid}/modifiers",
                          {"shape": "box", "size": [5, 5, 5], "config": {"sparse_infill_density": "15%"}})
        vid = (r.get("volume") or {}).get("id") if isinstance(r, dict) else None
        listed = next((v for v in _volumes(api, oid) if vid is not None and v.get("id") == vid), None)
        if status == 200 and listed and listed.get("type") == "modifier" and (listed.get("config") or {}).get("sparse_infill_density") == "15%":
            status, r = api.j("PUT", f"{base}/{listed['index']}/config", {"sparse_infill_density": "15%"})
            ok = status == 200 and "sparse_infill_density" in _applied_keys(r)
        detail = f"HTTP {status} {json.dumps(r)[:120]}"
    finally:
        for v in _volumes(api, oid):
            if vid is not None and v.get("id") == vid:
                api.j("DELETE", f"{base}/{v.get('index')}")
    ok = ok and len(_volumes(api, oid)) == len(before)
    check("control: modifier add, list, write-back, delete round-trip", ok, detail)


def run_checks(api: Api) -> list[tuple[str, bool, str]]:
    out: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = ""):
        out.append((name, bool(ok), detail))

    status, st = api.j("GET", "/status")
    check("status reachable", status == 200 and "app" in st, f"HTTP {status}")

    for key in FORBIDDEN_WRITES:
        status, r = guarded_write(api, {key: "1"})
        check(f"forbidden write refused: {key}", r.get("applied") == [] and r.get("errors", {}).get(key) == "forbidden_key",
              f"HTTP {status} {json.dumps(r)[:100]}")

    # Preferences > Remote API "Allow printer-type and connection-agent edits" is off by default:
    # a CHANGE to a policy-gated key is refused, while writing its current value back passes.
    _, cur = api.j("GET", "/config?keys=printer_agent")
    agent = cur.get("config", {}).get("printer_agent")
    status, r = guarded_write(api, {"printer_agent": "live-check-not-an-agent"})
    check("policy-gated change refused while the toggle is off",
          r.get("applied") == [] and r.get("errors", {}).get("printer_agent") == "blocked_by_remote_api_policy",
          f"HTTP {status} {json.dumps(r)[:120]}")
    if agent is not None:
        status, r = api.j("PUT", "/config", {"printer_agent": agent})
        check("control: policy-gated key written back unchanged passes", "printer_agent" in r.get("applied", []),
              f"HTTP {status} {json.dumps(r)[:120]}")

    # control: a *_gcode SWITCH is not code; only text-valued *_gcode templates are blocked
    _, cur = api.j("GET", "/config?keys=emit_machine_limits_to_gcode")
    emit = cur.get("config", {}).get("emit_machine_limits_to_gcode")
    if emit is not None:
        status, r = api.j("PUT", "/config", {"emit_machine_limits_to_gcode": emit})
        check("control: a *_gcode switch is not blocked as code", "emit_machine_limits_to_gcode" in r.get("applied", []),
              f"HTTP {status} {json.dumps(r)[:120]}")

    # control: an idempotent write must go through, so a refuse-everything bug cannot pass the checks above
    _, cur = api.j("GET", "/config?keys=layer_height")
    value = cur.get("config", {}).get("layer_height")
    status, r = api.j("PUT", "/config", {"layer_height": value})
    check("control: allowed key is accepted", "layer_height" in r.get("applied", []), f"HTTP {status} {json.dumps(r)[:100]}")

    printer = st.get("presets", {}).get("printer", "")
    status, r = api.j("POST", "/preset/config", {"type": "printer", "name": printer})
    cfg = r.get("config", {})
    hosts = {k: v for k, v in cfg.items() if k.startswith(("print_host", "printhost_")) or k in ("bbl_use_printhost", "host_type")}
    check("preset read redacts every print-host value", bool(hosts) and all(v == "<redacted>" for v in hosts.values()),
          f"{len(hosts)} keys, unredacted: {[k for k, v in hosts.items() if v != '<redacted>'][:4]}")

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
        refused_selections = [
            ("filament slot out of range refused", {"type": "filament", "name": slots[0], "extruder": 99}, 422),
            ("negative filament slot refused", {"type": "filament", "name": slots[0], "extruder": -1}, 400),
            ("extruder on a non-filament type refused", {"type": "print", "name": st["presets"]["print"], "extruder": 1}, 400)]
        for label, body, want in refused_selections:
            status, r = None, {}
            try:
                status, r = api.j("PUT", "/preset", body)
            finally:
                if status == 200:   # the guard did not hold and the selection may have changed: put the old one back
                    restore_selection(api, st)
            check(label, status == want and r.get("error") == "bad_extruder", f"HTTP {status} {r}")
    if slots:
        status, r = guarded_write(api, {"filament_slot": "1"})
        check("filament_slot of the wrong type refused", status == 400 and r.get("error") == "bad_filament_slot", f"HTTP {status} {r}")
        status, r = guarded_write(api, {"filament_slot": len(slots)})
        check("filament_slot out of range refused", status == 422 and r.get("error") == "bad_filament_slot", f"HTTP {status} {r}")
        out.extend(_filament_slot_write(api, slots))
    status, r = None, {}
    try:
        status, r = api.j("PUT", "/preset", {"type": "filament", "name": "No Such Filament Preset"})
    finally:
        if status == 200:
            restore_selection(api, st)
    check("unknown preset refused", status == 422 and r.get("error") == "unknown_preset", f"HTTP {status} {r}")

    status, r = api.j("GET", "/presets?type=bogus")
    check("presets filter rejects an unknown type", status == 400 and r.get("error") == "bad_param", f"HTTP {status} {r}")
    status, r = api.j("GET", "/presets?type=printer")
    check("presets filter returns only the requested collection", set(r) == {"printer"}, f"keys {sorted(r)}")

    _object_checks(api, check)

    bad = Api(api.base.rsplit("/api/v1", 1)[0], "not-the-token")
    status_bad, _ = bad.call("GET", "/status")
    check("wrong token is refused", status_bad in (401, 403), f"HTTP {status_bad}")
    return out


# Checks that only a working, permissive slicer passes (writes that must succeed, or plain reads). Every other
# check is a refusal check and must pass only the controls against a server that accepts everything, and fail only them against one that refuses everything.
CONTROLS = {
    "control: allowed key is accepted",
    "control: policy-gated key written back unchanged passes",
    "control: a *_gcode switch is not blocked as code",
    "control: re-selecting a slot's own filament succeeds and changes nothing",
    "control: modifier add, list, write-back, delete round-trip",
}
# Reads that succeed against both fakes: they say the fixture is alive, not that a guard works.
READS_THAT_PASS_EVERYWHERE = {"status reachable", "status lists filament slots", "fixture: an object is loaded", "object lists its volumes"}
# The fakes do not model per-slot nozzle_temperature, so this write check cannot run against them (it fails on both).
NOT_EXERCISED_BY_FAKES = {"filament_slot write changes only the slots sharing that preset"}
EXPECTED_PASS_WHEN_ACCEPTING = CONTROLS | READS_THAT_PASS_EVERYWHERE
EXPECTED_FAIL_WHEN_REFUSING = CONTROLS | NOT_EXERCISED_BY_FAKES

_CONFIG = {"layer_height": "0.2", "printer_agent": "orca", "emit_machine_limits_to_gcode": "0"}
_STATUS = {"app": "fake", "presets": {"printer": "P", "print": "Q", "filaments": ["F1", "F2"]}}


class _Fake(BaseHTTPRequestHandler):
    """Shared plumbing for the fake slicers."""
    HOST_VALUE = "<redacted>"
    def _send(self, status: int, body: dict):
        data = json.dumps(body).encode()
        self.send_response(status); self.send_header("Content-Type", "application/json"); self.send_header("Content-Length", str(len(data)))
        self.end_headers(); self.wfile.write(data)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return json.loads(self.rfile.read(n) or b"{}")

    def _vols(self) -> list[dict]:
        return [{"index": 0, "id": 70, "type": "part", "config": {}}]

    def _get_common(self):
        if self.path.startswith("/api/v1/status"):
            return self._send(200, _STATUS)
        if self.path.startswith("/api/v1/objects"):
            if self.path.rstrip("/").endswith("/objects"):
                return self._send(200, {"objects": [{"id": 7, "name": "cube"}], "count": 1})
            return self._send(200, {"id": 7, "config": {"print_host": self.HOST_VALUE}, "volumes": self._vols()})
        if self.path.startswith("/api/v1/config"):
            keys = (self.path.split("keys=", 1) + [""])[1].split(",")
            return self._send(200, {"config": {k: _CONFIG[k] for k in keys if k in _CONFIG}})
        return None

    def log_message(self, *_):
        pass


class _AcceptEverything(_Fake):
    """A broken 'slicer' that applies every write and accepts every token: the guard checks must fail against it."""
    HOST_VALUE = "http://u:p@h"
    def do_GET(self):
        if self._get_common() is None:
            self._send(200, {"printer": [], "filament": [], "print": []})

    def _vols(self) -> list[dict]:   # a modifier is always there, so the round-trip control finds it
        return super()._vols() + [{"index": 1, "id": 71, "type": "modifier", "config": {"sparse_infill_density": "15%"}}]

    def do_DELETE(self):
        self._send(200, {"deleted": True})

    def do_PUT(self):
        body = self._body()
        self._send(200, {"applied": list(body), "errors": {}, "selected": body.get("name"), "extruder": body.get("extruder", 0)})

    def do_POST(self):
        body = self._body()
        if self.path.endswith("/modifiers"):
            return self._send(200, {"volume": {"index": 1, "id": 71, "type": "modifier"}})
        if self.path.endswith("/preset/config"):
            return self._send(200, {"config": {"printhost_apikey": "SECRET", "print_host": "http://u:p@h"}})
        self._send(200, {"loaded": True, "path": body.get("path")})


class _RefuseEverything(_Fake):
    """A 'slicer' with every guard correct that also refuses every write, controls included: only the controls may fail."""
    def _deny(self, status: int, error: str):
        self._send(status, {"error": error, "applied": [], "errors": {}})

    def do_GET(self):
        if self.headers.get("X-Api-Token") != "t":
            return self._deny(401, "unauthorized")
        if self._get_common() is not None:
            return
        query = self.path.partition("type=")[2]
        if query == "printer":
            return self._send(200, {"printer": []})
        self._deny(400, "bad_param")

    def do_DELETE(self):
        self._deny(422, "cannot_delete_part")

    def do_PUT(self):
        body = self._body()
        if "/volumes/" in self.path:
            index = self.path.split("/volumes/", 1)[1].split("/", 1)[0]
            if not index.isdigit():
                return self._deny(400, "bad_volume_index")
            if int(index) != 0:
                return self._deny(404, "unknown_volume")
            errors = {k: "forbidden_key" if k == "post_process" else "blocked_by_remote_api_policy" if k == "printer_agent" else "refused"
                      for k in body}
            return self._send(200, {"applied": [], "errors": errors})
        if self.path.endswith("/config"):
            slot = body.get("filament_slot")
            if "filament_slot" in body:
                if not isinstance(slot, int):
                    return self._deny(400, "bad_filament_slot")
                return self._deny(422 if slot >= len(_STATUS["presets"]["filaments"]) else 403, "bad_filament_slot" if slot >= 2 else "refused")
            errors = {}
            for key in body:
                if key == "printer_agent":
                    errors[key] = "blocked_by_remote_api_policy"
                elif key not in _CONFIG:
                    errors[key] = "forbidden_key"
                else:
                    errors[key] = "refused"
            return self._send(200, {"applied": [], "errors": errors})
        if "extruder" in body and (body["extruder"] < 0 or body["extruder"] >= 2 or body.get("type") != "filament"):
            return self._deny(400 if body["extruder"] < 0 or body.get("type") != "filament" else 422, "bad_extruder")
        if body.get("name") == "No Such Filament Preset":
            return self._deny(422, "unknown_preset")
        self._deny(403, "refused")

    def do_POST(self):
        body = self._body()
        if self.path.endswith("/modifiers"):
            size = body.get("size", [])
            if not isinstance(body.get("shape"), str):
                return self._deny(400, "bad_modifier")
            if not size or any(not 0.1 <= n <= 1000 for n in size):
                return self._deny(400, "bad_modifier")
            return self._deny(422 if "post_process" in body.get("config", {}) else 403, "refused")
        if self.path.endswith("/preset/config"):
            return self._send(200, {"config": {"printhost_apikey": "<redacted>", "print_host": "<redacted>"}})
        path = str(body.get("path", ""))
        if not path.startswith("/") or path.startswith("//") or "\\" in path:
            return self._deny(400, "path_not_allowed")
        if not path.lower().endswith((".stl", ".3mf")):
            return self._deny(400, "unsupported_format")
        self._deny(404, "not_found")


class _QuietServer(HTTPServer):
    def handle_error(self, request, client_address):   # a client hanging up mid-response is not interesting here
        pass


def _run_against(handler: type[BaseHTTPRequestHandler]) -> list[tuple[str, bool, str]]:
    server = _QuietServer(("127.0.0.1", 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        return run_checks(Api(f"http://127.0.0.1:{server.server_port}", "t"))
    finally:
        server.shutdown()
        server.server_close()


def _compare(label: str, got: set[str], want: set[str]) -> bool:
    if got == want:
        return True
    print(f"selftest {label}: unexpected {sorted(got - want)}; missing {sorted(want - got)}")
    return False


def selftest() -> int:
    """The checker must detect broken guards (accept-everything) and a broken control (refuse-everything)."""
    accepted = _run_against(_AcceptEverything)
    refused = _run_against(_RefuseEverything)
    ok = _compare("accept-everything: checks that PASSED", {n for n, good, _ in accepted if good}, EXPECTED_PASS_WHEN_ACCEPTING)
    ok = _compare("refuse-everything: checks that FAILED", {n for n, good, _ in refused if not good}, EXPECTED_FAIL_WHEN_REFUSING) and ok
    if ok:
        print(f"selftest OK: accept-everything passed exactly {len(EXPECTED_PASS_WHEN_ACCEPTING)} of {len(accepted)} checks; "
              f"refuse-everything failed exactly {len(EXPECTED_FAIL_WHEN_REFUSING)} of {len(refused)}")
        return 0
    print("selftest FAILED: the checker cannot reliably tell broken guards or broken controls from working ones")
    return 1


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--url", default=os.environ.get("ORCA_API_URL", "http://127.0.0.1:13130"))
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
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
    except (KeyError, ValueError, TypeError, AttributeError) as exc:   # exit 1 is reserved for "a check failed"
        print(f"unexpected response shape from {args.url}, could not finish the checks: {type(exc).__name__}: {exc}", file=sys.stderr)
        return 2
    for name, ok, detail in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + ("" if ok else f"    [{detail}]"))
    failed = [n for n, ok, _ in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} checks passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
