#!/usr/bin/env bash
# build_board.sh — build one board variant into its own build dir.
#
#   tools/build_board.sh                 # reTerminal → firmware/build (release input)
#   tools/build_board.sh fnk0104s        # → firmware/build-fnk0104s
#   tools/build_board.sh amoled18        # → firmware/build-amoled18
#
# Each variant gets its own build dir AND its own sdkconfig, so building a
# colour board never rewrites firmware/sdkconfig (the reTerminal's).
# Builds run in espressif/idf:v5.4 via tools/idf.sh (TINY_IDF=host to opt out).
set -euo pipefail
IDF="$(cd "$(dirname "$0")" && pwd)/idf.sh"
board="${1:-reterminal}"
shift || true
case "$board" in
  reterminal)
    exec "$IDF" "$@" build ;;
  fnk0104s|amoled18)
    [ -f "$(dirname "$IDF")/../firmware/sdkconfig.defaults.$board" ] \
      || { echo "no sdkconfig.defaults.$board" >&2; exit 2; }
    exec "$IDF" -B "build-$board" -DSDKCONFIG="build-$board/sdkconfig" \
      -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.$board" "$@" build ;;
  *)
    echo "unknown board '$board' (reterminal | fnk0104s | amoled18)" >&2; exit 2 ;;
esac
