#!/usr/bin/env bash
# Install one of the stored 2.5 app-icon variants (resources/icons-2.5/<variant>)
# as the icons the build packages: resources/Icon.icns, resources/images/OrcaSlicer.icns
# and resources/images/OrcaSlicer.ico.
#   beta    : ribbon reads "2.5 Beta" (use while the base is a 2.5.0 beta)
#   release : ribbon reads "2.5"      (switch to this once the base is 2.5.0 stable)
unset TMOUT
set -euo pipefail

usage() { echo "usage: $0 beta|release" >&2; exit 2; }
[[ $# -eq 1 ]] || usage
variant=$1
[[ $variant == beta || $variant == release ]] || usage

root=$(cd "$(dirname "$0")/.." && pwd)
src=$root/resources/icons-2.5/$variant
for f in OrcaSlicer.icns OrcaSlicer.ico; do
  [[ -s $src/$f ]] || { echo "missing $src/$f" >&2; exit 1; }
done

cp "$src/OrcaSlicer.icns" "$root/resources/Icon.icns"
cp "$src/OrcaSlicer.icns" "$root/resources/images/OrcaSlicer.icns"
cp "$src/OrcaSlicer.ico"  "$root/resources/images/OrcaSlicer.ico"
echo "installed the $variant 2.5 icon"
