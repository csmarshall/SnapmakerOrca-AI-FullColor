"""Advisory analysers that work on model FILES rather than the live slicer: phyx3d printability and strength.

Results are guidance only (phyx3d labels each check exact / estimate / rough-guide and so do we). They never
change the slicer. The model file must live under an allowed directory, so a steered model cannot point the
analyser at arbitrary files.
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
from pathlib import Path
from typing import Any

from .client import ToolResult
from .spec import Tool

ALLOWED_SUFFIXES = {".stl", ".3mf"}
ANALYSER_TIMEOUT_S = 300
# phyx3d has Bambu printer presets only; the U1's bed and its multi-tool behaviour are not modelled.
CAVEAT = ("phyx3d models Bambu printers: ignore its bed-fit result for the Snapmaker U1 and treat everything "
          "as advisory (strength is 'rough guide', not certified engineering).")


def phyx3d_command() -> list[str] | None:
    """`PHYX3D_CLI` (path to dist/cli.js, run with node) wins; otherwise a `phyx3d` on PATH."""
    explicit = os.environ.get("PHYX3D_CLI")
    if explicit:
        return ["node", explicit] if Path(explicit).is_file() else None
    found = shutil.which("phyx3d")
    return [found] if found else None


def allowed_dirs() -> list[Path]:
    raw = os.environ.get("ORCA_AI_MODEL_DIRS")
    if raw:
        return [Path(d).resolve() for d in raw.split(os.pathsep) if d]
    cwd = Path.cwd().resolve()
    # A filesystem root or the home directory is not a sensible implicit sandbox: name a directory explicitly.
    return [] if cwd in (Path(cwd.anchor), Path.home().resolve()) else [cwd]


def _resolve_model(file: str) -> Path:
    path = Path(file).expanduser().resolve()
    if path.suffix.lower() not in ALLOWED_SUFFIXES:
        raise ValueError(f"only {', '.join(sorted(ALLOWED_SUFFIXES))} files can be analysed")
    if not any(path == d or d in path.parents for d in allowed_dirs()):
        raise ValueError(f"{path} is outside the allowed model directories (set ORCA_AI_MODEL_DIRS)")
    if not path.is_file():
        raise ValueError(f"{path} does not exist")
    return path


def _run(command: str, file: str, extra: list[str]) -> ToolResult:
    try:
        model = _resolve_model(file)
    except ValueError as exc:
        return ToolResult(str(exc), is_error=True)
    base = phyx3d_command()
    if base is None:
        return ToolResult("phyx3d not found: set PHYX3D_CLI to its dist/cli.js or put `phyx3d` on PATH", is_error=True)
    argv = [*base, command, str(model), "--json", "--no-save", *extra]
    try:
        done = subprocess.run(argv, capture_output=True, text=True, timeout=ANALYSER_TIMEOUT_S, stdin=subprocess.DEVNULL)
    except (subprocess.TimeoutExpired, OSError) as exc:
        return ToolResult(f"phyx3d failed: {exc}", is_error=True)
    if done.returncode not in (0, 2):  # phyx3d exits 2 when a check fails ("needs changes"); 1 is a real error
        return ToolResult(f"phyx3d exited {done.returncode}: {done.stderr[:500]}", is_error=True)
    try:
        report = json.loads(done.stdout)
    except json.JSONDecodeError:
        return ToolResult(f"phyx3d produced unreadable output: {done.stdout[:300]}", is_error=True)
    return ToolResult(json.dumps({"caveat": CAVEAT, **_condense(report)}))


def _condense(report: dict[str, Any]) -> dict[str, Any]:
    """Keep verdicts, summaries, fixes and the few numbers a model needs; drop per-voxel/hotspot bulk."""
    if isinstance(report, list):  # `orient` returns a ranked list of candidate orientations
        keep_o = ("rotation", "label", "status", "score", "supportArea", "islands", "tipAngle", "contactArea", "height", "warpScore")
        ranked = sorted((o for o in report if isinstance(o, dict)), key=lambda o: o.get("score", 0), reverse=True)
        return {"orientations": [{k: o[k] for k in keep_o if k in o} for o in ranked[:6]]}
    if "checks" in report:
        return {"verdict": report.get("verdict"), "score": report.get("score"), "headline": report.get("headline"),
                "checks": [{"id": c["id"], "status": c["status"], "accuracy": c.get("accuracy"),
                            "summary": c["summary"], "fixes": c.get("fixes", [])} for c in report["checks"]]}
    data = report.get("data", {})
    keep = ("minSafetyFactor", "weakestAt", "failureMode", "maxVonMises", "maxDeflection", "converged", "reliable")
    return {"status": report.get("status"), "accuracy": report.get("accuracy"), "summary": report.get("summary"),
            "fixes": report.get("fixes", []), "data": {k: data[k] for k in keep if k in data}}


_REGION = r"(?:[+-]?[xyz]|bottom|top|box:[0-9.,:+-]+)"
_REGION_RE = re.compile(rf"^{_REGION}$")
_FORCE_RE = re.compile(rf"^{_REGION}=[-+0-9.,eE]+$")


def _number(value: Any, name: str) -> float:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or value != value or abs(value) == float("inf"):
        raise ValueError(f"{name} must be a finite number")
    return value


def _region(value: Any, name: str) -> str:
    # A leading '-' is only legal as the axis form (-x); anything else starting with '-' would parse as a CLI flag.
    if not isinstance(value, str) or not _REGION_RE.match(value):
        raise ValueError(f"{name} entry {value!r} is not a valid region (x, -y, bottom, top, box:x,y,z:x,y,z)")
    return value


def _force(value: Any) -> str:
    if not isinstance(value, str) or not _FORCE_RE.match(value):
        raise ValueError(f"forces entry {value!r} must look like +x=0,0,-20 (region=fx,fy,fz in newtons)")
    return value


def _file_property() -> dict[str, Any]:
    return {"type": "string", "description": "Path to an .stl or .3mf model on disk (must be under an allowed directory)."}


def advisory_tools() -> list[Tool]:
    materials = ["PLA", "PETG", "ABS", "ASA", "TPU", "PLA-CF", "PETG-CF", "PC"]
    common = {"file": _file_property(),
              "material": {"type": "string", "enum": materials, "default": "PLA"},
              "infill_percent": {"type": "number", "description": "Sparse infill percent (default 15)."},
              "wall_loops": {"type": "integer", "description": "Wall loop count (default 2)."}}

    def options(a: dict[str, Any]) -> list[str]:
        # Every value reaches phyx3d's argv, so each is validated here rather than trusting the provider's schema.
        material = str(a.get("material", "PLA"))
        if material not in materials:
            raise ValueError(f"material must be one of {materials}")
        out = ["-m", material]
        if "infill_percent" in a: out += ["--infill", str(_number(a["infill_percent"], "infill_percent"))]
        if "wall_loops" in a: out += ["--walls", str(int(_number(a["wall_loops"], "wall_loops")))]
        if a.get("rotate_deg"):
            out += ["--rotate", ",".join(str(_number(v, "rotate_deg")) for v in a["rotate_deg"])]
        return out

    rotate = {"rotate_deg": {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3,
                             "description": "Rotate [x,y,z] degrees before analysis, to test an orientation."}}

    def check(a: dict[str, Any]) -> ToolResult:
        try:
            return _run("check", a["file"], options(a))
        except ValueError as exc:
            return ToolResult(str(exc), is_error=True)

    def strength(a: dict[str, Any]) -> ToolResult:
        try:
            fixed = [_region(r, "fixed") for r in a["fixed"]]
            forces = [_force(f) for f in a["forces"]]
            extra = options(a) + ["--fixed", *fixed, "--force", *forces]
            if "safety_factor" in a: extra += ["--safety", str(_number(a["safety_factor"], "safety_factor"))]
        except (ValueError, TypeError) as exc:
            return ToolResult(str(exc), is_error=True)
        return _run("stress", a["file"], extra)

    def orient(a: dict[str, Any]) -> ToolResult:
        try:
            return _run("orient", a["file"], options(a))
        except ValueError as exc:
            return ToolResult(str(exc), is_error=True)

    def tool(name: str, description: str, properties: dict[str, Any], required: list[str], fn: Any) -> Tool:
        return Tool(name=name, description=description + " Advisory, file-based. " + CAVEAT, method="LOCAL", path=name,
                    input_schema={"type": "object", "properties": properties, "required": required}, local=fn)

    return [
        tool("analyzePrintability",
             "Printability and stability checks on a model file (mesh health, overhangs and supports, floating islands, thin walls, tip-over, warp risk).",
             {**common, **rotate}, ["file"], check),
        tool("analyzeStrength",
             "Finite-element strength test: hold the part at `fixed` regions and apply `forces` in newtons; reports minimum safety factor, failure mode (including weak layer bonds) and deflection.",
             {**common, **rotate,
              "fixed": {"type": "array", "items": {"type": "string"}, "description": "Held regions, e.g. bottom, -x, box:0,0,0:10,10,5."},
              "forces": {"type": "array", "items": {"type": "string"}, "description": "region=fx,fy,fz in newtons, e.g. +x=0,0,-20."},
              "safety_factor": {"type": "number", "description": "Required safety factor (default 2)."}},
             ["file", "fixed", "forces"], strength),
        tool("rankOrientations",
             "Compare print orientations of a model and rank them by supports, islands, stability, warp and height.",
             common, ["file"], orient),
    ]
