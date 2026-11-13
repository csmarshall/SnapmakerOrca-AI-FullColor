#!/usr/bin/env python3
"""Generate candidates_u1_0.4.tsv for tests/libslic3r/test_spoolman_parent.cpp from the REAL system profiles. Not run by the build.

Usage (from the repo root):  python3 -I tests/data/spoolman/make_candidates.py > tests/data/spoolman/candidates_u1_0.4.tsv

Candidates = every instantiated filament preset that
  - lives in resources/profiles/Snapmaker/filament (not base/ subfolders' abstract presets) and has '@U1 0.4 nozzle' in its name, or
  - lives in resources/profiles/OrcaFilamentLibrary/filament and has an empty compatible_printers (visible on every printer),
with filament_type, filament_vendor, compatible_printers, alias, setting_id and filament_id resolved through `inherits` (child wins; the
first element of a list value is used, as for a single-extruder preset).
NOTE: the checked-in candidates_u1_0.4.tsv is a FROZEN snapshot of an earlier run (7th column and the Snapmaker ABS row added by hand);
regenerating it against the current tree changes ~50 expectations (the tree has since gained '@Snapmaker U1' Polymaker/Snapmaker presets and
renamed 'Snapmaker PETG HF'), a separate review. This is an APPROXIMATION of Orca's preset lookup. setting_id is taken from the child file only (inherits is not resolved for it), and
the same-library parent is preferred over the other library when a name exists in both; real Orca resolves per loaded bundle. Columns, tab separated, no header:
name, alias, filament_type, filament_vendor, setting_id, filament_id, printer_listed (1 when the resolved compatible_printers names
Snapmaker U1 (0.4 nozzle), else 0). Sorted by name so the file is stable.
"""
import json, os, sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "resources", "profiles")
PRINTER = "Snapmaker U1 (0.4 nozzle)"
DIRS = ["Snapmaker/filament", "OrcaFilamentLibrary/filament"]


def load_all():
    by_name = {}
    for d in DIRS:
        for dirpath, _, files in os.walk(os.path.join(ROOT, d)):  # includes base/ (the fdm_* parents)
            for fn in files:
                if fn.endswith(".json"):
                    with open(os.path.join(dirpath, fn), encoding="utf-8") as f:
                        j = json.load(f)
                    by_name.setdefault(j.get("name", fn[:-5]), []).append((d, j))
    return by_name


def resolve(j, d, by_name, key, depth=0):
    if key in j:
        return j[key]
    parent = j.get("inherits")
    if not parent or depth > 12:
        return None
    for d2, j2 in by_name.get(parent, []):
        # prefer the same library, fall back to the other (Snapmaker presets inherit OrcaFilamentLibrary ones)
        if d2 == d:
            return resolve(j2, d2, by_name, key, depth + 1)
    for d2, j2 in by_name.get(parent, []):
        return resolve(j2, d2, by_name, key, depth + 1)
    return None


def first(v):
    if isinstance(v, list):
        return str(v[0]) if v else ""
    return "" if v is None else str(v)


def main():
    by_name = load_all()
    rows = []
    for name, lst in by_name.items():
        for d, j in lst:
            if str(j.get("instantiation", "")).lower() != "true":
                continue
            if d.startswith("Snapmaker"):
                if "@U1 0.4 nozzle" not in name:
                    continue
            else:
                cp = resolve(j, d, by_name, "compatible_printers")
                if cp:
                    continue
            rows.append([name, first(resolve(j, d, by_name, "alias")), first(resolve(j, d, by_name, "filament_type")),
                         first(resolve(j, d, by_name, "filament_vendor")), first(j.get("setting_id")), first(resolve(j, d, by_name, "filament_id")),
                         "1" if PRINTER in (resolve(j, d, by_name, "compatible_printers") or []) else "0"])
    for r in sorted(rows):
        sys.stdout.write("\t".join(r) + "\n")


main()
