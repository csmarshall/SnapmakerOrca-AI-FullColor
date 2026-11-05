#!/usr/bin/env python3
"""Live regression check for the Remote API guards, run against a launched slicer (not a fake).

    ORCA_API_TOKEN=... python3 live_api_check.py [--url http://127.0.0.1:13130]
    python3 live_api_check.py --selftest     # detector check: must pass only the controls against a server that accepts everything, and fail only them against one that refuses everything

The tool-level checks (run_tool_checks: read-only tools leave /status unchanged, apply_and_slice, compare_slices, set_object_config,
set_layer_height, set_height_range, render_plate) DO change the scratch project (object config, layer height, a slice) and put it back afterwards.
The guard checks only send requests that are either refused or idempotent (a value written back unchanged), so it is safe on a
scratch profile. Exit code 0 = every guard held, 1 = at least one check failed, 2 = could not run (unreachable, no token, or an unexpected response shape).
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
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

    def raw(self, path: str) -> tuple[int, bytes]:
        """GET returning the undecoded body (for the PNG from /plate/render)."""
        req = urllib.request.Request(self.base + path, headers={"X-Api-Token": self.token})
        try:
            with urllib.request.urlopen(req, timeout=60) as r:
                return r.status, r.read()
        except urllib.error.HTTPError as e:
            return e.code, e.read()

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


TRANSFORM_GUARDS = [
    ("scale 0", {"scale": [0, 1, 1]}),
    ("translate 1e300", {"translate": [1e300, 0, 0]}),
    ("non-number element", {"rotate": [0, "x", 0]}),
    ("2-element vector", {"translate": [1, 2]}),
]


def _transform_guards(api: Api) -> list[tuple[str, bool, str]]:
    """Validation runs before the object lookup, so a nonexistent id still reaches bad_transform and a broken guard
    cannot move a real object. (NaN/Inf cannot be sent as strict JSON; they would come back as invalid_json.)"""
    out = []
    for label, body in TRANSFORM_GUARDS:
        status, r = api.j("POST", "/objects/999999999/transform", body)
        out.append((f"transform refuses {label}", status == 400 and r.get("error") == "bad_transform", f"HTTP {status} {json.dumps(r)[:100]}"))
    return out


TRANSFORM_SEMANTICS_CHECKS = [
    "transform: GET /objects reports rotation_deg beside rotation (radians)",
    "transform: translate is relative, in mm",
    "transform: rotate is relative, degrees in; rotation in radians, rotation_deg in degrees",
    "transform: scale is absolute (same factor twice gives the same scale)",
    "transform: response and GET /objects agree",
    "transform: bad vector refused (400 bad_transform)",
    "transform: unknown instance refused (404 unknown_instance)",
    "transform: original offset, rotation and scale restored",
]
_STEP_MM, _STEP_DEG, _TEST_SCALE = 5.0, 10.0, 1.25   # one named place for the probe sizes
_RAD_PER_DEG = 0.017453292519943295


def _first_instance(api: Api):
    _, r = api.j("GET", "/objects")
    for o in r.get("objects", []):
        if isinstance(o.get("transform"), dict):
            return o["id"], o["transform"]
    return None, None


def _transform_semantics(api: Api) -> list[tuple[str, bool, str]]:
    """Pins down what POST /objects/{id}/transform does (translate/rotate relative, scale absolute) and that rotations are
    reported in both radians and degrees. Every write is undone in the finally block."""
    n = TRANSFORM_SEMANTICS_CHECKS
    oid, t0 = _first_instance(api)
    if oid is None:
        return [(n[0], False, "no object with a transform in GET /objects")] + [(x, False, "no object") for x in n[1:]]
    out: list[tuple[str, bool, str]] = []
    path = f"/objects/{oid}/transform"
    close = lambda a, b, tol=1e-6: len(a) == len(b) and all(abs(x - y) <= tol for x, y in zip(a, b))
    out.append((n[0], isinstance(t0.get("rotation_deg"), list) and close(t0["rotation_deg"], [d / _RAD_PER_DEG for d in t0["rotation"]], 1e-4),
                f"transform {json.dumps(t0)[:160]}"))
    moved = False
    try:
        moved = True
        status, r = api.j("POST", path, {"translate": [_STEP_MM, 0, 0]})
        t1 = r.get("transform", {})
        out.append((n[1], status == 200 and close(t1.get("offset", []), [t0["offset"][0] + _STEP_MM, t0["offset"][1], t0["offset"][2]]),
                    f"HTTP {status} {json.dumps(r)[:140]}"))
        status, r = api.j("POST", path, {"rotate": [0, 0, _STEP_DEG]})
        t2 = r.get("transform", {})
        want_rad = [t0["rotation"][0], t0["rotation"][1], t0["rotation"][2] + _STEP_DEG * _RAD_PER_DEG]
        want_deg = [d / _RAD_PER_DEG for d in want_rad]
        out.append((n[2], status == 200 and close(t2.get("rotation", []), want_rad) and close(t2.get("rotation_deg", []), want_deg, 1e-4),
                    f"HTTP {status} {json.dumps(r)[:160]} want rad {want_rad}"))
        scales = []
        for _ in range(2):
            status, r = api.j("POST", path, {"scale": [_TEST_SCALE] * 3})
            scales.append(r.get("transform", {}).get("scale", []))
        out.append((n[3], status == 200 and all(close(s, [_TEST_SCALE] * 3) for s in scales), f"scales {scales}"))
        _, now = _first_instance(api)
        out.append((n[4], now is not None and close(now["rotation"], t2.get("rotation", [])) and close(now["offset"], t2.get("offset", []))
                    and close(now["scale"], scales[-1]), f"GET {json.dumps(now)[:140]}"))
        status, r = api.j("POST", path, {"translate": [1, 2]})
        out.append((n[5], status == 400 and r.get("error") == "bad_transform", f"HTTP {status} {json.dumps(r)[:100]}"))
        status, r = api.j("POST", path, {"translate": [1, 0, 0], "instance": 9999})
        out.append((n[6], status == 404 and r.get("error") == "unknown_instance", f"HTTP {status} {json.dumps(r)[:100]}"))
    finally:
        if moved:   # inverse translate, inverse rotate, original scale: the handler adds per component, so this is exact
            api.j("POST", path, {"translate": [-_STEP_MM, 0, 0]})
            api.j("POST", path, {"rotate": [0, 0, -_STEP_DEG]})
            api.j("POST", path, {"scale": t0["scale"]})
    _, back = _first_instance(api)
    out.append((n[7], back is not None and close(back["offset"], t0["offset"], 1e-6) and close(back["rotation"], t0["rotation"], 1e-9)
                and close(back["scale"], t0["scale"], 1e-9), f"before {json.dumps(t0)[:100]} after {json.dumps(back)[:100]}"))
    return out


def _dirty_print_checks(api: Api, st: dict) -> list[tuple[str, bool, str]]:
    """PUT /preset discard:true drops unsaved edits in EVERY group, so refuse to run while anything is modified."""
    out: list[tuple[str, bool, str]] = []
    # extruder_nozzle_stats#N is the app's own per-extruder count of nozzle types, derived from the printer setup;
    # the app marks it modified on load, so it is not a person's unsaved work and does not block these checks.
    modified = {g: [k for k in v if not str(k).startswith("extruder_nozzle_stats")]
                for g, v in (st.get("modified") or {}).items()}
    modified = {g: v for g, v in modified.items() if v}
    names = ["PUT /preset refuses to drop unsaved edits (409 unsaved_changes)", "PUT /preset rejects a non-boolean discard (400 bad_discard)",
             "control: PUT /preset discard:true drops the print edit and restores the saved value",
             "POST /preset/discard rejects an unknown type (400 unknown_type)",
             "control: POST /preset/discard drops the print edit and restores the saved value"]
    if modified:
        return [(n, False, f"unsaved edits present {modified}; refusing to discard them") for n in names]
    printp = st.get("presets", {}).get("print", "")
    key = "wall_loops"
    _, cur = api.j("GET", f"/config?keys={key}")
    orig = str(cur.get("config", {}).get(key, ""))
    if not orig.lstrip("-").isdigit():
        return [(n, False, f"{key} is not an integer: {orig!r}") for n in names]
    edited = str(int(orig) + 1)

    def dirty() -> int:
        return api.j("PUT", "/config", {key: edited})[0]

    def value() -> str:
        return str(api.j("GET", f"/config?keys={key}")[1].get("config", {}).get(key, ""))

    leaked = False
    try:
        dirty()
        status, r = api.j("PUT", "/preset", {"type": "print", "name": printp})
        leaked |= status == 200
        out.append((names[0], status == 409 and r.get("error") == "unsaved_changes" and "print" in r.get("dirty", []), f"HTTP {status} {json.dumps(r)[:120]}"))
        status, r = api.j("PUT", "/preset", {"type": "print", "name": printp, "discard": "yes"})
        leaked |= status == 200
        out.append((names[1], status == 400 and r.get("error") == "bad_discard", f"HTTP {status} {json.dumps(r)[:120]}"))
        dirty()
        status, r = api.j("PUT", "/preset", {"type": "print", "name": printp, "discard": True})
        _, st3 = api.j("GET", "/status")
        out.append((names[2], status == 200 and "print" in r.get("discarded_changes", []) and value() == orig
                    and not (st3.get("modified") or {}).get("print"), f"HTTP {status} {json.dumps(r)[:120]} value {value()!r} want {orig!r}"))
        status, r = api.j("POST", "/preset/discard", {"type": "bogus"})
        out.append((names[3], status == 400 and r.get("error") == "unknown_type", f"HTTP {status} {json.dumps(r)[:100]}"))
        dirty()
        status, r = api.j("POST", "/preset/discard", {"type": "print"})
        _, st4 = api.j("GET", "/status")
        out.append((names[4], status == 200 and r.get("discarded") is True and key in r.get("dropped_keys", []) and value() == orig
                    and not (st4.get("modified") or {}).get("print"), f"HTTP {status} {json.dumps(r)[:120]} value {value()!r} want {orig!r}"))
    finally:   # put the saved value back whatever happened, and re-select if a refusal let a selection through
        if value() != orig:
            api.j("PUT", "/config", {key: orig})
        if leaked:
            restore_selection(api, st)
    return out
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
    out.extend(_transform_guards(api))
    out.extend(_transform_semantics(api))
    out.extend(_dirty_print_checks(api, st))
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
# Both fakes model the transform endpoint (relative translate/rotate, absolute scale) for the fixture object, so these agree with the fakes everywhere.
# That shows the check and the fakes agree, not that the C++ handler is right: only a live run against a build shows that.
READS_THAT_PASS_EVERYWHERE |= set(TRANSFORM_SEMANTICS_CHECKS)
# The fakes do not model per-slot nozzle_temperature, so this write check cannot run against them (it fails on both).
NOT_EXERCISED_BY_FAKES = {"filament_slot write changes only the slots sharing that preset"}
EXPECTED_PASS_WHEN_ACCEPTING = CONTROLS | READS_THAT_PASS_EVERYWHERE
# Pass paths that only a real slicer can show (a fake that refuses everything cannot drop an edit).
CONTROLS_NEEDING_A_REAL_SLICER = {
    "control: PUT /preset discard:true drops the print edit and restores the saved value",
    "control: POST /preset/discard drops the print edit and restores the saved value",
}
EXPECTED_FAIL_WHEN_REFUSING = CONTROLS | NOT_EXERCISED_BY_FAKES | CONTROLS_NEEDING_A_REAL_SLICER

_CONFIG = {"layer_height": "0.2", "printer_agent": "orca", "emit_machine_limits_to_gcode": "0", "wall_loops": "2"}
_STATUS = {"app": "fake", "presets": {"printer": "P", "print": "Q", "filaments": ["F1", "F2"]},
           "modified": {"print": [], "filament": [], "printer": []}}


# ---------------------------------------------------------------------------------------------------------------------------------
# Tool-level checks (#4, #5). The MCP tool names come from MaxEllis orcaslicer-mcp; these checks drive the REST calls behind them:
#   get_preset_config -> POST /preset/config     apply_and_slice -> PUT /config, POST /slice, poll GET /slice/status
#   compare_slices    -> two apply_and_slice runs, compare GET /slice/status stats
#   set_object_config -> PUT /objects/{id}/config   set_layer_height -> PUT /objects/{id}/layer_height
#   set_height_range  -> PUT /objects/{id}/height_range   render_plate -> GET /plate/render?angle=...
# The mapping is inferred from openapi.yaml and the tool names, not read from the MCP source (not available locally).
PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
RENDER_ANGLES = ["iso", "top", "front", "left", "right", "rear", "bottom"]
SLICE_TIMEOUT_S = 180


def _names(coll) -> list[str]:
    return [x if isinstance(x, str) else str(x.get("name", "")) for x in (coll or [])]


def _wait_slice(api: Api, timeout: float = SLICE_TIMEOUT_S) -> dict:
    """Start a slice and poll until it is done or errors. Returns the final /slice/status body (or {"state": "timeout"})."""
    status, r = api.j("POST", "/slice")
    if status not in (200, 202):
        return {"state": "start_failed", "message": f"HTTP {status} {json.dumps(r)[:80]}"}
    end = time.time() + timeout
    st: dict = {}
    while time.time() < end:
        _, st = api.j("GET", "/slice/status")
        if st.get("state") in ("done", "error"):
            return st
        time.sleep(0.2 if timeout < 30 else 1.0)
    return {"state": "timeout", **st}


def _read_only_calls(api: Api, oid) -> list[tuple[str, str, str, object]]:
    """(tool, method, path, body) for every read-only call. get_preset_config is aimed at presets OTHER than the selected ones,
    because the field report (#4) was a printer preset changing after it."""
    _, st = api.j("GET", "/status")
    cur = st.get("presets", {})
    calls = [("getStatus", "GET", "/status", None), ("getConfig", "GET", "/config?keys=layer_height,wall_loops", None),
             ("getObjects", "GET", "/objects", None), ("getObject", "GET", f"/objects/{oid}", None),
             ("getPresets", "GET", "/presets?visible=1", None), ("getSliceStatus", "GET", "/slice/status", None),
             ("getPlates", "GET", "/plates", None), ("getJobsStatus", "GET", "/jobs/status", None),
             ("getGcode", "GET", "/gcode", None)]
    for typ, selected in (("printer", cur.get("printer")), ("print", cur.get("print")), ("filament", (cur.get("filaments") or [None])[0])):
        _, pr = api.j("GET", f"/presets?type={typ}&visible=1")
        others = [n for n in _names(pr.get(typ)) if n and n != selected][:2]
        calls += [("getPresetConfig", "POST", "/preset/config", {"type": typ, "name": n}) for n in others]
        if selected:
            calls.append(("getPresetConfig", "POST", "/preset/config", {"type": typ, "name": selected}))
    return calls


def run_tool_checks(api: Api) -> list[tuple[str, bool, str]]:
    out: list[tuple[str, bool, str]] = []

    def check(name: str, ok: bool, detail: str = ""):
        out.append((name, bool(ok), detail))

    status, objs = api.j("GET", "/objects")
    olist = objs.get("objects", []) if isinstance(objs, dict) else []
    check("tool fixture: an object is loaded", status == 200 and bool(olist), f"HTTP {status}")
    if not olist:
        return out
    oid = olist[0]["id"]

    # (a) every read-only tool leaves getStatus identical. Both snapshots come from the same endpoint, so this catches a read that
    # changes what /status reports; it cannot see state that /status does not report (selected-but-unlisted edits, GUI state).
    for tool, method, path, body in _read_only_calls(api, oid):
        _, before = api.j("GET", "/status")
        code, _txt = api.call(method, path, body)
        _, after = api.j("GET", "/status")
        label = f"read-only {tool}" + (f" {body['type']}:{body['name']}" if body else "")
        diff = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
        check(f"status unchanged by {label}", before == after and code < 500, f"HTTP {code}; /status changed in {diff}")

    # (b) set_object_config
    _, o0 = api.j("GET", f"/objects/{oid}")
    prev = (o0.get("config") or {}).get("wall_loops")
    s1, r1 = api.j("PUT", f"/objects/{oid}/config", {"wall_loops": 4})
    _, o1 = api.j("GET", f"/objects/{oid}")
    check("tool set_object_config: value applied and read back", s1 == 200 and str((o1.get("config") or {}).get("wall_loops")) == "4",
          f"HTTP {s1} {json.dumps(r1)[:80]} read back {(o1.get('config') or {}).get('wall_loops')}")
    s2, r2 = api.j("PUT", f"/objects/{oid}/config", {"wall_loops": 6, "not_a_real_key": 1})
    _, o2 = api.j("GET", f"/objects/{oid}")
    check("tool set_object_config: a bad key applies nothing (atomic)", s2 >= 400 and str((o2.get("config") or {}).get("wall_loops")) == "4",
          f"HTTP {s2} wall_loops now {(o2.get('config') or {}).get('wall_loops')}")
    s3, r3 = api.j("PUT", f"/objects/{oid}/config", {"post_process": "x"})
    _, o3 = api.j("GET", f"/objects/{oid}")
    check("tool set_object_config: forbidden key refused", s3 >= 400 and "post_process" not in (o3.get("config") or {}), f"HTTP {s3} {json.dumps(r3)[:80]}")
    _, g = api.j("GET", "/config?keys=wall_loops")
    api.j("PUT", f"/objects/{oid}/config", {"wall_loops": prev if prev is not None else g.get("config", {}).get("wall_loops", 2)})

    # (c) set_layer_height
    s, r = api.j("PUT", f"/objects/{oid}/layer_height", {"mode": "adaptive", "quality": 0.5})
    check("tool set_layer_height: adaptive accepted", s == 200 and "error" not in r, f"HTTP {s} {json.dumps(r)[:80]}")
    s, r = api.j("PUT", f"/objects/{oid}/layer_height", {"mode": "adaptive", "quality": 7})
    check("tool set_layer_height: quality out of range refused", s >= 400, f"HTTP {s} {json.dumps(r)[:80]}")
    s, r = api.j("PUT", f"/objects/{oid}/layer_height", {"mode": "sideways"})
    check("tool set_layer_height: unknown mode refused", s >= 400, f"HTTP {s} {json.dumps(r)[:80]}")
    s, r = api.j("PUT", f"/objects/{oid}/layer_height", {"mode": "reset"})
    check("tool set_layer_height: reset accepted", s == 200 and "error" not in r, f"HTTP {s} {json.dumps(r)[:80]}")

    # (d) set_height_range
    s, r = api.j("PUT", f"/objects/{oid}/height_range", {"min_z": 0, "max_z": 2, "layer_height": 0.1})
    check("tool set_height_range: range accepted", s == 200 and "error" not in r, f"HTTP {s} {json.dumps(r)[:80]}")
    for label, body in (("min above max", {"min_z": 5, "max_z": 1, "layer_height": 0.1}), ("layer_height 0", {"min_z": 0, "max_z": 2, "layer_height": 0}),
                        ("layer_height negative", {"min_z": 0, "max_z": 2, "layer_height": -0.2})):
        s, r = api.j("PUT", f"/objects/{oid}/height_range", body)
        check(f"tool set_height_range: {label} refused", s >= 400, f"HTTP {s} {json.dumps(r)[:80]}")
    s, r = api.j("PUT", f"/objects/{oid}/height_range", {"clear": True})
    check("tool set_height_range: clear accepted", s == 200 and "error" not in r, f"HTTP {s} {json.dumps(r)[:80]}")

    # (e) render_plate: a real PNG for every angle, in both views
    for angle in RENDER_ANGLES:
        code, body = api.raw(f"/plate/render?view=editor&angle={angle}&width=320&height=240")
        check(f"tool render_plate: editor {angle} is a PNG", code == 200 and body[:8] == PNG_MAGIC and len(body) > 100, f"HTTP {code} {len(body)} bytes {body[:8]!r}")
    code, body = api.raw("/plate/render?angle=sideways")
    check("tool render_plate: unknown angle refused", code >= 400, f"HTTP {code}")

    # (f) apply_and_slice and compare_slices. The first slice is not asserted on its own beyond "done with stats": with the same
    # model, a larger layer height must give fewer layers and a shorter estimate, which is what makes the two runs distinguishable.
    _, g = api.j("GET", "/config?keys=layer_height")
    orig_lh = g.get("config", {}).get("layer_height", "0.2")
    results = {}
    for lh in ("0.2", "0.4"):
        s, r = api.j("PUT", "/config", {"layer_height": lh})
        applied = "layer_height" in _applied_keys(r)
        sl = _wait_slice(api)
        stats = sl.get("stats") or {}
        results[lh] = stats.get("estimated_time_seconds")
        check(f"tool apply_and_slice: layer_height {lh} applied, slice done with stats",
              s == 200 and applied and sl.get("state") == "done" and bool(stats.get("estimated_time_seconds")),
              f"PUT HTTP {s} applied={applied} state={sl.get('state')} {sl.get('message', '')} stats={stats}")
    a, b = results.get("0.2"), results.get("0.4")
    check("tool compare_slices: 0.4 mm layers estimate shorter than 0.2 mm", bool(a and b and b < a), f"0.2 -> {a}s, 0.4 -> {b}s")
    api.j("PUT", "/config", {"layer_height": orig_lh})
    return out


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

    def _xf(self) -> dict:
        """The fixture object's single instance: offset mm, Euler rotation in RADIANS, scale factor (per server, like the app)."""
        return self.server.state.setdefault("xf", {"offset": [10.0, 20.0, 0.0], "rotation": [0.0, 0.0, 0.1], "scale": [1.0, 1.0, 1.0]})

    def _xf_view(self) -> dict:
        x = self._xf()
        return {**x, "rotation_deg": [r / _RAD_PER_DEG for r in x["rotation"]]}

    def _apply_transform(self, body: dict) -> tuple[int, dict]:
        """Mirrors the C++ handler: vectors of 3 finite numbers (400 bad_transform), instance 0 only (404 unknown_instance),
        translate/rotate ADDED (mm, degrees in), scale REPLACED. Scale must be positive."""
        limits = {"translate": (-10000, 10000), "rotate": (-3600, 3600), "scale": (1e-4, 1000)}
        vecs = {}
        for k, (lo, hi) in limits.items():
            if k in body:
                v = body[k]
                if (not isinstance(v, list) or len(v) != 3 or any(isinstance(n, bool) or not isinstance(n, (int, float)) for n in v)
                        or any(not lo <= n <= hi for n in v)):
                    return 400, {"error": "bad_transform"}
                vecs[k] = [float(n) for n in v]
        if not vecs:
            return 400, {"error": "no_transform"}
        inst = body.get("instance", 0)
        if not isinstance(inst, int) or isinstance(inst, bool):
            return 400, {"error": "instance_must_be_integer"}
        if inst != 0:
            return 404, {"error": "unknown_instance", "instances": 1}
        x = self._xf()
        if "translate" in vecs:
            x["offset"] = [a + b for a, b in zip(x["offset"], vecs["translate"])]
        if "rotate" in vecs:
            x["rotation"] = [a + b * _RAD_PER_DEG for a, b in zip(x["rotation"], vecs["rotate"])]
        if "scale" in vecs:
            x["scale"] = vecs["scale"]
        return 200, {"id": 7, "instance": 0, "transform": self._xf_view()}

    def _get_common(self):
        if self.path.startswith("/api/v1/status"):
            return self._send(200, _STATUS)
        if self.path.startswith("/api/v1/objects"):
            if self.path.rstrip("/").endswith("/objects"):
                return self._send(200, {"objects": [{"id": 7, "name": "cube", "transform": self._xf_view()}], "count": 1})
            return self._send(200, {"id": 7, "config": {"print_host": self.HOST_VALUE}, "volumes": self._vols()})
        if self.path.startswith("/api/v1/config"):
            keys = (self.path.split("keys=", 1) + [""])[1].split(",")
            live = {**_CONFIG, **getattr(self.server, "state", {})}   # fixture writes overlay the static config
            return self._send(200, {"config": {k: live[k] for k in keys if k in live}})
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
        if self.path == "/api/v1/objects/7/transform":
            return self._send(*self._apply_transform(body))
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
            if set(body) == {"wall_loops"}:   # the dirty-print fixture write, not a control: a real slicer applies it, and it is what the 409 checks need
                self.server.state["wall_loops"] = str(body["wall_loops"])
                return self._send(200, {"applied": ["wall_loops"], "errors": {}})
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
        if body.get("type") == "print":
            if "discard" in body and not isinstance(body["discard"], bool):
                return self._deny(400, "bad_discard")
            if body.get("discard") is True:
                return self._deny(403, "refused")   # control: a real slicer drops the edit here
            if self.server.state.get("wall_loops", _CONFIG["wall_loops"]) != _CONFIG["wall_loops"]:
                return self._send(409, {"error": "unsaved_changes", "dirty": ["print"], "applied": [], "errors": {}})
        self._deny(403, "refused")

    def do_POST(self):
        body = self._body()
        if self.path == "/api/v1/objects/7/transform":
            return self._send(*self._apply_transform(body))
        if "/transform" in self.path:
            return self._deny(400, "bad_transform")
        if self.path.endswith("/preset/discard"):
            return self._deny(400 if body.get("type") not in ("print", "filament", "printer") else 403,
                              "unknown_type" if body.get("type") not in ("print", "filament", "printer") else "refused")
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
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.state: dict = {}   # fixture writes, per server, so the two fakes never share edits

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


class _Stateful(_Fake):
    """A fake slicer that KEEPS state, so the tool-level checks have something to be wrong about. It models a printer selection,
    a global layer height, one object with a config and layer-height state, and a slice whose estimate depends on the layer height.
    Set `server.state["bugs"]` to inject a fault: nozzle_flip (a read-only call changes the selected printer, the #4 report),
    slice_ignores_config (the estimate never changes), objconfig_not_stored, render_not_png, height_range_accepts_bad."""
    PRINTERS = {"U1 0.2 nozzle": "0.2", "U1 0.4 nozzle": "0.4"}

    def _st(self) -> dict:
        st = self.server.state
        st.setdefault("bugs", set())
        st.setdefault("printer", "U1 0.2 nozzle")
        st.setdefault("lh", "0.2")
        st.setdefault("obj", {})
        st.setdefault("sliced", False)
        return st

    def _bug(self, name: str) -> bool:
        return name in self._st()["bugs"]

    def do_GET(self):
        st = self._st()
        path = self.path.split("?")[0]
        if path == "/api/v1/status":
            return self._send(200, {"app": "fake", "presets": {"printer": st["printer"], "print": "Q", "filaments": ["F1"]},
                                    "modified": {"print": [], "filament": [], "printer": []}, "slicing": False, "slice_result_valid": st["sliced"]})
        if path == "/api/v1/objects":
            return self._send(200, {"objects": [{"id": 7, "name": "mushroom"}], "count": 1})
        if path == "/api/v1/objects/7":
            return self._send(200, {"id": 7, "config": dict(st["obj"]), "volumes": [{"index": 0, "type": "part", "config": {}}]})
        if path == "/api/v1/config":
            keys = (self.path.split("keys=", 1) + [""])[1].split(",")
            live = {"layer_height": st["lh"], "wall_loops": "2"}
            return self._send(200, {"config": {k: live[k] for k in keys if k in live}})
        if path == "/api/v1/presets":
            typ = (self.path.split("type=", 1) + [""])[1].split("&")[0]
            return self._send(200, {"printer": list(self.PRINTERS), "print": ["Q", "Q2"], "filament": ["F1", "F2"]} if not typ else
                              {typ: {"printer": list(self.PRINTERS), "print": ["Q", "Q2"], "filament": ["F1", "F2"]}[typ]})
        if path == "/api/v1/slice/status":
            if not st["sliced"]:
                return self._send(200, {"state": "idle", "percent": -1, "message": "", "warnings": []})
            secs = 1000 if self._bug("slice_ignores_config") else int(2000 / float(st["lh"]) / 10)
            return self._send(200, {"state": "done", "percent": -1, "message": "", "warnings": [], "stats": {"estimated_time_seconds": secs}})
        if path in ("/api/v1/plates", "/api/v1/jobs/status"):
            return self._send(200, {"plates": [], "jobs": []})
        if path == "/api/v1/gcode":
            return self._send(409 if not st["sliced"] else 200, {"error": "no_slice"})
        if path == "/api/v1/plate/render":
            angle = (self.path.split("angle=", 1) + [""])[1].split("&")[0]
            if angle not in RENDER_ANGLES:
                return self._send(400, {"error": "bad_angle"})
            data = (b"not a png" if self._bug("render_not_png") else PNG_MAGIC) + b"\0" * 200
            self.send_response(200); self.send_header("Content-Type", "image/png"); self.send_header("Content-Length", str(len(data)))
            self.end_headers(); self.wfile.write(data)
            return None
        return self._send(404, {"error": "not_found"})

    def do_PUT(self):
        st, body = self._st(), self._body()
        path = self.path.split("?")[0]
        if path == "/api/v1/config":
            if "layer_height" in body:
                st["lh"] = str(body["layer_height"]); st["sliced"] = False
            return self._send(200, {"applied": list(body), "errors": {}})
        if path == "/api/v1/objects/7/config":
            bad = [k for k in body if k == "post_process" or k.startswith("not_a_real")]
            if bad:
                return self._send(422, {"error": "invalid_keys", "errors": {k: "refused" for k in bad}})
            if not self._bug("objconfig_not_stored"):
                st["obj"].update({k: str(v) for k, v in body.items()})
            return self._send(200, {"applied": list(body), "errors": {}})
        if path == "/api/v1/objects/7/layer_height":
            if body.get("mode") not in ("adaptive", "reset") or not 0 <= body.get("quality", 0.5) <= 1:
                return self._send(400, {"error": "bad_layer_height"})
            return self._send(200, {"mode": body["mode"]})
        if path == "/api/v1/objects/7/height_range":
            if body.get("clear"):
                return self._send(200, {"cleared": True})
            ok = body.get("layer_height", 0) > 0 and body.get("min_z", 0) < body.get("max_z", 0)
            if not ok and not self._bug("height_range_accepts_bad"):
                return self._send(400, {"error": "bad_range"})
            return self._send(200, {"ranges": 1})
        return self._send(404, {"error": "not_found"})

    def do_POST(self):
        st, body = self._st(), self._body()
        if self.path == "/api/v1/slice":
            st["sliced"] = True
            return self._send(202, {"started": True})
        if self.path == "/api/v1/preset/config":
            if self._bug("nozzle_flip") and body.get("type") == "printer":
                st["printer"] = body.get("name", st["printer"])   # the fault: reading a preset selects it
            return self._send(200, {"config": {"nozzle_diameter": self.PRINTERS.get(body.get("name"), "0.4")}})
        return self._send(404, {"error": "not_found"})

    def do_DELETE(self):
        self._send(404, {"error": "not_found"})


# Which tool checks each injected fault must break (and nothing else). Clean fake: none.
TOOL_FAULTS = {
    # both: reading the other printer flips the selection, and reading the originally selected one then flips it back (also a change)
    "nozzle_flip": {"status unchanged by read-only getPresetConfig printer:U1 0.4 nozzle", "status unchanged by read-only getPresetConfig printer:U1 0.2 nozzle"},
    "slice_ignores_config": {"tool compare_slices: 0.4 mm layers estimate shorter than 0.2 mm"},
    "objconfig_not_stored": {"tool set_object_config: value applied and read back", "tool set_object_config: a bad key applies nothing (atomic)"},
    "render_not_png": {f"tool render_plate: editor {a} is a PNG" for a in RENDER_ANGLES},
    "height_range_accepts_bad": {f"tool set_height_range: {x} refused" for x in ("min above max", "layer_height 0", "layer_height negative")},
}


def _run_tools_against(bug: str | None) -> list[tuple[str, bool, str]]:
    server = _QuietServer(("127.0.0.1", 0), _Stateful)
    server.state["bugs"] = {bug} if bug else set()
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        return run_tool_checks(Api(f"http://127.0.0.1:{server.server_port}", "t"))
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
    clean = _run_tools_against(None)
    ok = _compare("tool checks, clean stateful fake: FAILED", {n for n, good, _ in clean if not good}, set()) and ok
    for bug, expect in TOOL_FAULTS.items():
        ok = _compare(f"tool checks, injected fault {bug}: FAILED", {n for n, good, _ in _run_tools_against(bug) if not good}, expect) and ok
    if ok:
        print(f"selftest OK: accept-everything passed exactly {len(EXPECTED_PASS_WHEN_ACCEPTING)} of {len(accepted)} checks; "
              f"refuse-everything failed exactly {len(EXPECTED_FAIL_WHEN_REFUSING)} of {len(refused)}; "
              f"{len(clean)} tool checks pass on a clean stateful fake and each of {len(TOOL_FAULTS)} injected faults breaks exactly its own checks")
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
        api = Api(args.url, token)
        results = run_checks(api) + run_tool_checks(api)
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
