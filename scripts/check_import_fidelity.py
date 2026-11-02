#!/usr/bin/env python3
"""Checks that a CLI slice of a multi-plate, adaptive-layer-height 3MF kept both.

Morning procedure (issue #58), grid.3mf = baseline250/in/grid.3mf (two plates, adaptive layers):
  1. <2.4.1 build> --slice 0 --outputdir OUT_241 grid.3mf     then   python3 -I check_import_fidelity.py OUT_241   -> must PASS
  2. <2.5 build>   --slice 0 --outputdir OUT_25  grid.3mf     then   python3 -I check_import_fidelity.py OUT_25    -> FAIL before the fix, PASS after
  3. python3 -I check_import_fidelity.py --self-test
Step 1 proves the checker accepts known-good output; it cannot prove 2.5 output is printable.
Input: a directory holding plate_N.gcode files (searched recursively), or a .gcode.3mf with Metadata/plate_N.gcode.
Checks: (i) >= 2 plates; (ii) each plate names a different object; (iii) layer heights vary after the first layer
(a flat slice has one distinct height; flat layers also mean the count matches a flat 0.2 mm slice). Exits 1 with one line per failure.
"""
import os
import re
import sys
import zipfile

FLAT_LAYER = 0.2
PLATE_RE = re.compile(r"plate_(\d+)\.gcode$")
OBJECT_RE = re.compile(r"^(?:EXCLUDE_OBJECT_DEFINE\b.*?\bNAME=(\S+).*|; printing object (.+?)\s*)$")
HEIGHT_RE = re.compile(r"^;\s*HEIGHT:\s*([0-9.]+)")


def load_plates(path):
    plates = {}
    if os.path.isdir(path):
        for root, _dirs, files in os.walk(path):
            for f in files:
                m = PLATE_RE.search(f)
                if m:
                    with open(os.path.join(root, f), errors="replace") as fh:
                        plates[int(m.group(1))] = fh.read()
    elif zipfile.is_zipfile(path):
        with zipfile.ZipFile(path) as z:
            for n in z.namelist():
                m = PLATE_RE.search(n)
                if m:
                    plates[int(m.group(1))] = z.read(n).decode(errors="replace")
    return plates


def objects_of(text):
    names = set()
    for line in text.splitlines():
        m = OBJECT_RE.match(line)
        if m:
            names.add((m.group(1) or m.group(2)).split(" id:")[0])
    return names


def heights_of(text):
    return [float(m.group(1)) for line in text.splitlines() if (m := HEIGHT_RE.match(line))]


def check(plates):
    fails = []
    if len(plates) < 2:
        fails.append(f"only {len(plates)} plate G-code file(s) found, expected at least 2")
    seen = {}
    for n in sorted(plates):
        objs = objects_of(plates[n])
        if not objs:
            fails.append(f"plate {n}: no object names (EXCLUDE_OBJECT_DEFINE / '; printing object') in G-code")
        seen[n] = objs
    ns = sorted(seen)
    for i, a in enumerate(ns):
        for b in ns[i + 1:]:
            if seen[a] and seen[a] == seen[b]:
                fails.append(f"plates {a} and {b} print the same objects: {sorted(seen[a])}")
    for n in ns:
        hs = heights_of(plates[n])
        if len(hs) < 2:
            fails.append(f"plate {n}: no ;HEIGHT: lines found")
            continue
        rest = hs[1:]
        if len({round(h, 3) for h in rest}) < 2:
            fails.append(f"plate {n}: layer height is flat ({rest[0]} mm) after the first layer, adaptive profile lost")
    return fails


def gcode(objs, heights):
    head = "".join(f"EXCLUDE_OBJECT_DEFINE NAME={o} CENTER=1,1\n" for o in objs)
    return head + "".join(f";HEIGHT:{h}\nG1 X1 Y1\n" for h in heights)


def self_test():
    good = {1: gcode(["Non_Hardened_113"], [0.2, 0.18, 0.15, 0.12, 0.2]),
            2: gcode(["Hardened_114"], [0.2, 0.19, 0.14, 0.12, 0.2])}
    bad = {1: gcode(["Non_Hardened_113", "Hardened_114"], [0.2] * 6)}  # known-bad: one plate, flat layers
    assert check(good) == [], check(good)
    f = check(bad)
    assert any("plate G-code" in x for x in f) and any("flat" in x for x in f), f
    assert objects_of("; printing object Foo id:0 copy 0") == {"Foo"}
    print("self-test OK")


if __name__ == "__main__":
    if sys.argv[1:] == ["--self-test"]:
        self_test()
        sys.exit(0)
    if len(sys.argv) != 2:
        sys.exit("usage: check_import_fidelity.py <output dir | .gcode.3mf> | --self-test")
    p = load_plates(sys.argv[1])
    failures = check(p)
    for line in failures:
        print("FAIL: " + line)
    print("PASS" if not failures else f"{len(failures)} failure(s)")
    sys.exit(1 if failures else 0)
