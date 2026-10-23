#!/bin/bash
# release.sh — turn firmware/build into a web-installable release.
#
#   tools/release.sh            # reuse firmware/build if its version matches tiny_version.h
#   tools/release.sh --build    # force a build first (tools/idf.sh: the pinned ESP-IDF image)
#   tools/release.sh --deploy   # …then `mkdocs gh-deploy --force` (binaries ride along)
#   tools/release.sh --vendor-ewt  # refresh docs/js/esp-web-tools from npm (Apache-2.0)
#
# Produces (committed, small):
#   docs/install/manifest.json     ESP Web Tools manifest, parts under fw/ (reTerminal Sticky;
#                                  install.sh and the home page read this one)
#   docs/install/sha256sums.txt    one line per part, sha256sum(1) format
#   docs/install/manifest-<board>.json + sha256sums-<board>.txt, parts under fw/<board>/,
#                                  for every board variant whose build dir exists
#                                  (firmware/build-fnk0104s, firmware/build-amoled18;
#                                  tools/build_board.sh <board>). The install page's board
#                                  picker reads these. manifest-reterminal.json = manifest.json.
# The binaries themselves are NOT committed. tools/install_assets_hook.py copies
# them from firmware/build* into site/install/fw/ at mkdocs build time, so a
# `mkdocs gh-deploy` publishes exactly the bytes the manifests describe.
#
# STICKY_RELEASE_OUT=<dir> writes the manifests somewhere else (tests).
set -euo pipefail
DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$DIR/firmware/build"
OUT="${STICKY_RELEASE_OUT:-$DIR/docs/install}"
VER=$(sed -n 's/.*TINY_FW_VERSION *"\([^"]*\)".*/\1/p' "$DIR/firmware/main/tiny/tiny_version.h")
[ -n "$VER" ] || { echo "release: cannot read TINY_FW_VERSION" >&2; exit 1; }

DO_BUILD=0; DO_DEPLOY=0; DO_VENDOR=0
for a in "$@"; do
  case "$a" in
    --build) DO_BUILD=1 ;;
    --deploy) DO_DEPLOY=1 ;;
    --vendor-ewt) DO_VENDOR=1 ;;
    *) echo "release: unknown flag $a" >&2; exit 2 ;;
  esac
done

# Parts and flash settings come straight from IDF's own flasher_args.json (single source).

# Does the build dir carry this version? IDF writes the app descriptor into the
# binary; `esptool image_info` is heavy, so grep the version string instead.
build_matches() {
  [ -f "$BUILD/flasher_args.json" ] && [ -f "$BUILD/tiny_sticky.bin" ] \
    && grep -aq "$VER" "$BUILD/tiny_sticky.bin"
}

if [ "$DO_BUILD" = 1 ] || ! build_matches; then
  echo "release: building $VER (tools/idf.sh: espressif/idf:v5.4 unless TINY_IDF=host)"
  "$DIR/tools/idf.sh" reconfigure >/dev/null && "$DIR/tools/idf.sh" build
  build_matches || { echo "release: build does not contain $VER" >&2; exit 1; }
else
  echo "release: firmware/build already carries $VER — reusing"
fi

mkdir -p "$OUT"
# One manifest per board. $1 build dir, $2 manifest name, $3 sums name, $4 fw/ subdir, $5 board.
write_manifest() {
  python3 - "$1" "$OUT" "$VER" "$2" "$3" "$4" "$5" <<'PY'
import hashlib, json, os, sys
build, out, ver, mname, sname, sub, board = sys.argv[1:8]
fa = json.load(open(os.path.join(build, "flasher_args.json")))
cfg = json.load(open(os.path.join(build, "config", "sdkconfig.json")))
built_for = [k[len("TINY_BOARD_"):].lower() for k, v in cfg.items() if k.startswith("TINY_BOARD_") and v is True]
if built_for != [board]:
    sys.exit(f"release: {build} is configured for {built_for}, not {board}")
prefix = f"fw/{sub}/" if sub else "fw/"
manifest_parts, sums = [], []
for off, rel in sorted(fa["flash_files"].items(), key=lambda kv: int(kv[0], 16)):
    name = os.path.basename(rel)
    data = open(os.path.join(build, rel), "rb").read()
    manifest_parts.append({"path": prefix + name, "offset": int(off, 16)})
    sums.append(f"{hashlib.sha256(data).hexdigest()}  {prefix}{name}\n")
    print(f"release:   {board:<10} {off:>8}  {len(data):>9} B  {prefix}{name}")
manifest = {
    "name": "Sticky",
    "version": ver,
    "board": board,
    "flash_settings": fa.get("flash_settings", {}),
    "new_install_prompt_erase": True,
    "new_install_improv_wait_time": 0,
    "builds": [{"chipFamily": "ESP32-S3", "parts": manifest_parts}],
}
with open(os.path.join(out, mname), "w") as f:
    json.dump(manifest, f, indent=2); f.write("\n")
with open(os.path.join(out, sname), "w") as f:
    f.writelines(sums)
print(f"release: wrote {out}/{mname} + {sname} for {board} {ver}")
PY
}
write_manifest "$BUILD" manifest.json sha256sums.txt "" reterminal
cp "$OUT/manifest.json" "$OUT/manifest-reterminal.json"
cp "$OUT/sha256sums.txt" "$OUT/sha256sums-reterminal.txt"
for b in fnk0104s amoled18; do
  bd="$DIR/firmware/build-$b"
  if [ -f "$bd/flasher_args.json" ] && grep -aq "$VER" "$bd/tiny_sticky.bin"; then
    write_manifest "$bd" "manifest-$b.json" "sha256sums-$b.txt" "$b" "$b"
  else
    echo "release: no $VER build for $b (tools/build_board.sh $b) — its install manifest is left as it was"
  fi
done

# The two "Install Sticky <small>vX</small>" buttons are the only version strings
# a human reads on the site. They are static HTML, so rewrite them here — 0.28.0
# shipped with the buttons still saying 0.27.0-u2 because nothing did.
if [ -n "${STICKY_RELEASE_OUT:-}" ]; then
  echo "release: STICKY_RELEASE_OUT set — install buttons left alone"
else
for f in "$DIR/docs/overrides/home.html" "$DIR/docs/install/index.md"; do
  sed -i.bak -E "s#(Install Sticky <small>)v[^<]*(</small>)#\1v$VER\2#" "$f" && rm -f "$f.bak"
done
grep -q "<small>v$VER</small>" "$DIR/docs/install/index.md" || { echo "release: install button did not take v$VER" >&2; exit 1; }
echo "release: install buttons say v$VER"
fi

if [ "$DO_VENDOR" = 1 ]; then
  # refresh the vendored ESP Web Tools bundle (Apache-2.0) used by docs/install/index.md
  EWT="$DIR/docs/js/esp-web-tools"; T=$(mktemp -d)
  TARBALL=$(curl -fsSL https://registry.npmjs.org/esp-web-tools/latest | python3 -c 'import json,sys;print(json.load(sys.stdin)["dist"]["tarball"])')
  curl -fsSL "$TARBALL" | tar xz -C "$T"
  find "$EWT" -name '*.js' -delete
  cp "$T"/package/dist/web/*.js "$EWT"/ && cp "$T"/package/LICENSE "$EWT"/LICENSE
  V=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["version"])' "$T/package/package.json")
  sed -i.bak "s/esp-web-tools [0-9][0-9.]*/esp-web-tools $V/" "$EWT/README.md" && rm -f "$EWT/README.md.bak"
  rm -rf "$T"; echo "release: vendored esp-web-tools $V"
fi

if [ "$DO_DEPLOY" = 1 ]; then
  echo "release: mkdocs gh-deploy --force"
  ( cd "$DIR" && mkdocs gh-deploy --force )
fi
