#!/bin/bash
# gen_commit host test — the SHIPPED firmware/main/cmake/gen_commit.cmake run
# with `cmake -P` against a throwaway git repo laid out like this one. The
# -dirty suffix must cover everything compiled into the image (firmware/main,
# the project CMakeLists, sdkconfig defaults, partition tables, the vendored
# Seeed components) and nothing that is not (docs, tools, gitignored build dirs).
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
GEN="$ROOT/firmware/main/cmake/gen_commit.cmake"
command -v cmake >/dev/null || { echo "gen_commit host test: needs cmake"; exit 1; }
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
R="$T/repo"
mkdir -p "$R/firmware/main/cmake" "$R/vendor/Sticky_dashboard_demo/components/seeed_epaper" "$R/docs" "$R/tools"
printf 'main\n' >"$R/firmware/main/tiny_node.cpp"
printf 'project\n' >"$R/firmware/CMakeLists.txt"
printf 'nvs,data,nvs,0x9000,0x6000\n' >"$R/firmware/partitions_16mb.csv"
printf 'CONFIG_X=y\n' >"$R/firmware/sdkconfig.defaults.fnk0104s"
printf 'drv\n' >"$R/vendor/Sticky_dashboard_demo/components/seeed_epaper/epaper_panel.c"
printf 'doc\n' >"$R/docs/x.md"
printf 'tool\n' >"$R/tools/x.sh"
printf 'firmware/build-*/\nfirmware/sdkconfig\n' >"$R/.gitignore"
git -C "$R" init -q
git -C "$R" -c user.email=t@t -c user.name=t add -A
GIT_COMMITTER_DATE="2026-10-24T13:05:00+02:00" git -C "$R" -c user.email=t@t -c user.name=t commit -qm init
SHA="$(git -C "$R" rev-parse --short=8 HEAD)"

stamp() {
  rm -f "$T/out.h"
  cmake -DREPO_DIR="$R/firmware/main" -DOUT="$T/out.h" -P "$GEN"
  sed -n 's/.*TINY_FW_COMMIT "\(.*\)"/\1/p' "$T/out.h"
}
FAILS=0
expect() {   # <what> <file to touch, relative to repo> <want: clean|dirty>
  local what="$1" f="$2" want="$3" got
  git -C "$R" checkout -q -- . && git -C "$R" clean -qfd
  if [ -n "$f" ]; then mkdir -p "$(dirname "$R/$f")"; printf 'edit\n' >>"$R/$f"; fi
  got="$(stamp)"
  local ok=0
  case "$want" in clean) [ "$got" = "$SHA" ] && ok=1 ;; dirty) [ "$got" = "$SHA-dirty" ] && ok=1 ;; esac
  if [ "$ok" = 1 ]; then echo "  ok   $what → $got"; else echo "  FAIL $what → $got (want $want)"; FAILS=$((FAILS + 1)); fi
}
expect "clean tree"                         ""                                                            clean
expect "docs edit (not compiled)"           docs/x.md                                                     clean
expect "tools edit (not compiled)"          tools/x.sh                                                    clean
expect "gitignored build dir"               firmware/build-fnk0104s/x.bin                                 clean
expect "firmware/main source"               firmware/main/tiny_node.cpp                                   dirty
expect "untracked firmware/main file"       firmware/main/new.cpp                                         dirty
expect "project CMakeLists (outside main)"  firmware/CMakeLists.txt                                       dirty
expect "partition table (outside main)"     firmware/partitions_16mb.csv                                  dirty
expect "sdkconfig defaults (outside main)"  firmware/sdkconfig.defaults.fnk0104s                          dirty
expect "vendored component"                 vendor/Sticky_dashboard_demo/components/seeed_epaper/epaper_panel.c dirty
# main's TINY_FW_COMMIT_DATE survives the widened dirty scope: committer date, in UTC.
git -C "$R" checkout -q -- . && git -C "$R" clean -qfd
stamp >/dev/null
DATE="$(sed -n 's/.*TINY_FW_COMMIT_DATE "\(.*\)"/\1/p' "$T/out.h")"
if [ "$DATE" = "2026-10-24 11:05 UTC" ]; then echo "  ok   commit date → $DATE"; else echo "  FAIL commit date → '$DATE' (want 2026-10-24 11:05 UTC)"; FAILS=$((FAILS + 1)); fi
[ "$FAILS" = 0 ] || { echo "gen_commit host test: $FAILS FAILED"; exit 1; }
echo "gen_commit host test: ALL GREEN"
