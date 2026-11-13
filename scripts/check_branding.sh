#!/usr/bin/env bash
# Branding guard for the OpenSnorca rebrand (docs/records/rebrand-opensnorca.md in the plan repo).
#
#   scripts/check_branding.sh                 source check: user-visible product strings
#   scripts/check_branding.sh --app <x.app>   bundle check: Info.plist, executable, URL scheme, icon
#
# Source check: every gettext literal (_L/L/_u8L/_CTX/_CTX_utf8/_utf8/_) and every standalone "OrcaSlicer"
# string literal in src/ that still says OrcaSlicer / Orca Slicer must be listed in
# scripts/rebrand_allowlist.txt (format: <path-substring><TAB><text-substring>; '#' comments).
# Cannot catch: product names built at runtime, strings in resources/web or images, wording that is merely
# wrong. It proves "no un-reviewed product string", not "every string reads well".
set -euo pipefail
unset TMOUT
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ALLOW="$ROOT/scripts/rebrand_allowlist.txt"
EXPECT_NAME="OpenSnorca"
EXPECT_ID="com.wozi.opensnorca"
EXPECT_SCHEME="opensnorca"

check_source() {
  python3 -I - "$ROOT" "$ALLOW" <<'PY'
import re, sys, pathlib
root, allow_path = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
allow = []
if allow_path.exists():
    for ln in allow_path.read_text(encoding="utf-8").splitlines():
        if ln.strip() and not ln.startswith("#"):
            p, _, t = ln.partition("\t")
            allow.append((p, t))
tok = re.compile(r"OrcaSlicer|Orca Slicer")
call = re.compile(r'\b(?:_L|L|_u8L|_CTX|_CTX_utf8|_utf8|_L_PLURAL|_)\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)')
solo = re.compile(r'(?<![A-Za-z_])L?"OrcaSlicer"')
hits, total = [], 0
for f in sorted((root / "src").rglob("*")):
    if f.suffix not in (".cpp", ".hpp", ".h", ".mm"):
        continue
    rel = str(f.relative_to(root)); txt = f.read_text(encoding="utf-8", errors="replace")
    found = []
    for m in call.finditer(txt):
        lit = m.group(1)
        if tok.search(lit):
            found.append((txt.count("\n", 0, m.start(1)) + 1, "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', lit))))
    for m in solo.finditer(txt):
        found.append((txt.count("\n", 0, m.start()) + 1, m.group(0)))
    for line, text in found:
        total += 1
        if not any(p in rel and t in text for p, t in allow):
            hits.append((rel, line, text[:110]))
for h in hits:
    print("%s:%d: %s" % h)
print("branding source check: %d product-string literals, %d not allowlisted" % (total, len(hits)))
sys.exit(1 if hits else 0)
PY
}

check_app() {
  local app="$1" plist="$1/Contents/Info.plist" rc=0
  fail() { echo "FAIL: $*"; rc=1; }
  plutil -lint "$plist" >/dev/null || fail "plutil -lint"
  local name id exe
  name="$(plutil -extract CFBundleName raw "$plist")"; id="$(plutil -extract CFBundleIdentifier raw "$plist")"
  exe="$(plutil -extract CFBundleExecutable raw "$plist")"
  [[ "$name" == "$EXPECT_NAME" ]] || fail "CFBundleName=$name"
  [[ "$id" == "$EXPECT_ID" ]] || fail "CFBundleIdentifier=$id"
  [[ -f "$app/Contents/MacOS/$exe" ]] || fail "CFBundleExecutable '$exe' not in Contents/MacOS"
  [[ "$(basename "$app")" == "$EXPECT_NAME.app" ]] || fail "bundle directory is $(basename "$app")"
  local schemes; schemes="$(plutil -extract CFBundleURLTypes xml1 -o - "$plist")"
  grep -q "<string>$EXPECT_SCHEME</string>" <<<"$schemes" || fail "URL scheme $EXPECT_SCHEME missing"
  grep -qi "<string>orcaslicer" <<<"$schemes" && fail "URL scheme orcaslicer* still registered"
  [[ -f "$app/Contents/Resources/Icon.icns" ]] || fail "Icon.icns missing"
  echo "bundle: name=$name id=$id exe=$exe schemes ok=$([[ $rc == 0 ]] && echo yes || echo no)"
  return $rc
}

if [[ "${1:-}" == "--app" ]]; then check_app "${2:?path to .app}"; else check_source; fi
