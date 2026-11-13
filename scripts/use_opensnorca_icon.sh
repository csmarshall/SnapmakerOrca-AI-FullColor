#!/usr/bin/env bash
# Install the OpenSnorca app icon (resources/icons-opensnorca/) over the files the build and the GUI load by
# their historical OrcaSlicer names (file names are referenced from ~15 places, so only the content changes).
# The SVG logos (About, splash, gradient, horizontal) are NOT regenerated here.
unset TMOUT
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
src=$root/resources/icons-opensnorca
img=$root/resources/images
for f in OpenSnorca.icns OpenSnorca.ico icon_1024.png icon_512.png icon_256.png icon_128.png; do
  [[ -s $src/$f ]] || { echo "missing $src/$f" >&2; exit 1; }
done
cp "$src/OpenSnorca.icns" "$root/resources/Icon.icns"
cp "$src/OpenSnorca.icns" "$img/OrcaSlicer.icns"
cp "$src/OpenSnorca.ico"  "$img/OrcaSlicer.ico"
# Resample the 1024 master (sips keeps the alpha channel). Same pixel sizes as the files they replace.
resize() { sips -z "$2" "$2" "$src/icon_1024.png" --out "$img/$1" >/dev/null; }
resize OrcaSlicer.png 154
resize OrcaSlicer_154.png 154
resize OrcaSlicerTitle.png 154
resize OrcaSlicer_154_title.png 184
resize OrcaSlicer-mac_128px.png 128
resize OrcaSlicer_128px.png 128
resize OrcaSlicer_192px.png 192
resize OrcaSlicer_192px_transparent.png 192
resize OrcaSlicer_32px.png 32
resize OrcaSlicer_64.png 64
# Greyscale variant (error dialogs): desaturate the 192px logo.
resize OrcaSlicer_192px_grayscale.png 192
magick "$img/OrcaSlicer_192px_grayscale.png" -colorspace Gray -alpha on "$img/OrcaSlicer_192px_grayscale.png" 2>/dev/null \
  || magick "$img/OrcaSlicer_192px_grayscale.png" -modulate 100,0 "$img/OrcaSlicer_192px_grayscale.png"
python3 -I - "$src" "$img" <<'PY'
import struct, sys, pathlib
src, img = map(pathlib.Path, sys.argv[1:3])
def ico(png: pathlib.Path, size: int) -> bytes:
    data = png.read_bytes()
    return struct.pack("<HHH", 0, 1, 1) + struct.pack("<BBBBHHII", size % 256, size % 256, 0, 0, 1, 32, len(data), 22) + data
(img / "OrcaSlicer-mac_256px.ico").write_bytes(ico(src / "icon_256.png", 256))
(img / "OrcaSlicerTitle.ico").write_bytes(ico(src / "icon_128.png", 128))
PY
echo "installed the OpenSnorca icon"
