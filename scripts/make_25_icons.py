#!/usr/bin/env python3
"""Regenerate the app icons with a '2.5' corner badge.

Sources are read from git (BASE_REF) so the script is idempotent: running it
again never badges an already-badged icon.  Needs Pillow, iconutil (macOS).

Usage: scripts/make_25_icons.py [PREVIEW_DIR]
Writes resources/Icon.icns, resources/images/OrcaSlicer.icns and
resources/images/OrcaSlicer.ico; optional PREVIEW_DIR gets a PNG per size.
"""
import io
import subprocess
import sys
import tempfile
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

BASE_REF = "u25/stack"
BADGE_TEXT = "2.5"
BADGE_FILL = (214, 40, 40, 255)      # high-contrast red
BADGE_TEXT_COLOR = (255, 255, 255, 255)
BADGE_RING = (255, 255, 255, 255)
FONT_CANDIDATES = [
    "/System/Library/Fonts/Supplemental/Arial Bold.ttf",
    "/System/Library/Fonts/Helvetica.ttc",
]
ICNS_SIZES = [16, 32, 64, 128, 256, 512, 1024]
ICNS_FILES = ["resources/Icon.icns", "resources/images/OrcaSlicer.icns"]
ICO_FILE = "resources/images/OrcaSlicer.ico"
ICO_SIZES = [16, 32, 48, 64, 128, 256]

ROOT = Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip())


def git_blob(path: str) -> bytes:
    return subprocess.check_output(["git", "show", f"{BASE_REF}:{path}"], cwd=ROOT)


def font(px: int) -> ImageFont.FreeTypeFont:
    for f in FONT_CANDIDATES:
        if Path(f).exists():
            return ImageFont.truetype(f, px, index=1 if f.endswith(".ttc") and "Helvetica" in f else 0)
    raise SystemExit("no bold font found")


def badge(im: Image.Image) -> Image.Image:
    """Return im with the badge drawn in the lower-right corner."""
    size = im.width
    ss = 4                                   # supersample for smooth edges
    big = im.convert("RGBA").resize((size * ss, size * ss), Image.LANCZOS)
    d = ImageDraw.Draw(big)
    w, h = int(size * 0.62 * ss), int(size * 0.34 * ss)
    margin = int(size * 0.02 * ss)
    x1, y1 = big.width - margin, big.height - margin
    x0, y0 = x1 - w, y1 - h
    ring = max(ss, int(size * 0.025 * ss))
    d.rounded_rectangle([x0 - ring, y0 - ring, x1 + ring, y1 + ring], radius=h // 2 + ring, fill=BADGE_RING)
    d.rounded_rectangle([x0, y0, x1, y1], radius=h // 2, fill=BADGE_FILL)
    f = font(int(h * 0.80))
    d.text(((x0 + x1) / 2, (y0 + y1) / 2), BADGE_TEXT, font=f, fill=BADGE_TEXT_COLOR, anchor="mm")
    return big.resize((size, size), Image.LANCZOS)


def largest_from_icns(data: bytes) -> Image.Image:
    with tempfile.TemporaryDirectory() as t:
        src = Path(t) / "in.icns"
        src.write_bytes(data)
        out = Path(t) / "in.iconset"
        subprocess.check_call(["iconutil", "-c", "iconset", "-o", str(out), str(src)])
        return Image.open(out / "icon_512x512@2x.png").convert("RGBA")


def write_icns(master: Image.Image, dest: Path) -> None:
    with tempfile.TemporaryDirectory() as t:
        iconset = Path(t) / "out.iconset"
        iconset.mkdir()
        for s in (16, 32, 128, 256, 512):
            badge(master.resize((s, s), Image.LANCZOS)).save(iconset / f"icon_{s}x{s}.png")
            badge(master.resize((s * 2, s * 2), Image.LANCZOS)).save(iconset / f"icon_{s}x{s}@2x.png")
        subprocess.check_call(["iconutil", "-c", "icns", "-o", str(dest), str(iconset)])


def main() -> None:
    preview = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    master = largest_from_icns(git_blob(ICNS_FILES[0]))   # 1024 px
    for rel in ICNS_FILES:
        m = largest_from_icns(git_blob(rel))
        write_icns(m, ROOT / rel)
    ico_master = Image.open(io.BytesIO(git_blob(ICO_FILE))).convert("RGBA")
    ico_master = ico_master.resize((256, 256), Image.LANCZOS)
    frames = [badge(ico_master.resize((s, s), Image.LANCZOS)) for s in ICO_SIZES]
    frames[-1].save(ROOT / ICO_FILE, format="ICO", sizes=[(s, s) for s in ICO_SIZES], append_images=frames[:-1])
    if preview:
        preview.mkdir(parents=True, exist_ok=True)
        for s in ICNS_SIZES:
            badge(master.resize((s, s), Image.LANCZOS)).save(preview / f"icon_25_{s}px.png")


if __name__ == "__main__":
    main()
