#!/usr/bin/env python3
"""Static consistency check for the plate (BedType) tables, stdlib only.

    python3 tools/tests/check_bed_types.py [project.3mf]

Asserts that the Snapmaker "Graphic Effect Plate" is declared everywhere a plate must be (enum, key map, the two
temperature options, the filament option list, print invalidation) and that every plate name the U1 machine profiles'
G-code compares curr_bed_type with is a key the C++ table knows. With a .3mf, also checks that the project's
curr_bed_type and every *_plate_temp* key are known.

It parses the same sources it checks, so it proves the tables are declared and agree with each other and with the
profiles, NOT that 3mf loading or slicing works. The profile-literal check (e) is the most independent part.
"""
from __future__ import annotations

import glob
import json
import os
import re
import sys
import zipfile

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
GESP_KEY, GESP_ENUM = "Graphic Effect Plate", "btGESP"
# Dead in Snapmaker 2.4.1 too (its key map has no such name); kept in the profile G-code, never emitted by curr_bed_type.
LEGACY_DEAD_LITERALS = {"Smooth PEI Plate"}
GESP_TEMP_KEYS = ["graphic_effect_plate_temp", "graphic_effect_plate_temp_initial_layer"]


def read(rel: str) -> str:
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def parse(hpp: str, cpp: str, preset_cpp: str, print_cpp: str):
    m = re.search(r"enum BedType\s*\{(.*?)\}", hpp, re.S)
    enum = [x.strip() for x in re.sub(r"//[^\n]*", "", m.group(1)).split(",") if x.strip()] if m else []
    m = re.search(r"s_keys_map_BedType\s*=\s*\{(.*?)\};", cpp, re.S)
    keymap = dict(re.findall(r'\{\s*"([^"]+)"\s*,\s*(\w+)\s*\}', m.group(1))) if m else {}
    m = re.search(r'this->add\("curr_bed_type".*?set_default_value', cpp, re.S)
    enum_values = re.findall(r'enum_values\.emplace_back\("([^"]+)"\)', m.group(0)) if m else []
    defs = set(re.findall(r'this->add\("([a-z_]*plate_temp[a-z_]*)"', cpp))
    return enum, keymap, enum_values, defs


def run(three_mf: str | None = None) -> list[tuple[str, bool, str]]:
    hpp, cpp = read("src/libslic3r/PrintConfig.hpp"), read("src/libslic3r/PrintConfig.cpp")
    preset_cpp, print_cpp = read("src/libslic3r/Preset.cpp"), read("src/libslic3r/Print.cpp")
    enum, keymap, enum_values, defs = parse(hpp, cpp, preset_cpp, print_cpp)
    out = []
    out.append(("(a) enum BedType contains btGESP", GESP_ENUM in enum, f"enum {enum}"))
    out.append(("(b) s_keys_map_BedType maps 'Graphic Effect Plate' to btGESP", keymap.get(GESP_KEY) == GESP_ENUM, f"map {keymap}"))
    for k in GESP_TEMP_KEYS:
        out.append((f"(c) option {k} is declared", k in defs, f"declared {sorted(defs)}"))
    for k in GESP_TEMP_KEYS:
        out.append((f"(d) {k} is in the filament option list (Preset.cpp)", f'"{k}"' in preset_cpp, ""))
    for k in GESP_TEMP_KEYS:
        out.append((f"(d2) {k} is in the Print.cpp invalidation lists", f'"{k}"' in print_cpp, ""))
    out.append(("(d3) get_bed_temp_key and get_bed_temp_1st_layer_key handle btGESP", hpp.count("btGESP") >= 3, f"btGESP appears {hpp.count('btGESP')} times in PrintConfig.hpp"))
    literals: dict[str, set[str]] = {}
    for path in sorted(glob.glob(os.path.join(ROOT, "resources/profiles/Snapmaker/machine/Snapmaker U1*.json"))):
        for name in re.findall(r'curr_bed_type\s*==\s*\\"([^"\\]+)\\"', open(path, encoding="utf-8").read()):
            literals.setdefault(name, set()).add(os.path.basename(path))
    unknown = {n: sorted(f) for n, f in literals.items() if n not in keymap and n not in LEGACY_DEAD_LITERALS}
    out.append(("(e) every curr_bed_type literal in the U1 machine profiles is a key of s_keys_map_BedType",
                bool(literals) and not unknown, f"literals {sorted(literals)} unknown to the C++ table: {unknown}"))
    order_ok = bool(enum_values) and all(enum.index(keymap.get(v, "?")) == i + 1 if keymap.get(v) in enum else False
                                         for i, v in enumerate(enum_values))
    out.append(("(f) curr_bed_type enum_values follow BedType enum order from btPC", order_ok, f"values {enum_values} enum {enum}"))
    if three_mf:
        with zipfile.ZipFile(three_mf) as z:
            cfg = json.loads(z.read("Metadata/project_settings.config"))
        bed = cfg.get("curr_bed_type")
        out.append((f"3mf: curr_bed_type {bed!r} is a known plate", bed in keymap, f"known {sorted(keymap)}"))
        plate_keys = [k for k in cfg if "plate_temp" in k]
        unknown_keys = [k for k in plate_keys if k not in defs]
        out.append(("3mf: every *plate_temp* key is a known option", not unknown_keys, f"unknown {unknown_keys}"))
    return out


def main() -> int:
    results = run(sys.argv[1] if len(sys.argv) > 1 else None)
    for name, ok, detail in results:
        print(f"{'PASS' if ok else 'FAIL'}  {name}" + ("" if ok else f"    [{detail[:200]}]"))
    bad = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(bad)}/{len(results)} checks passed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
