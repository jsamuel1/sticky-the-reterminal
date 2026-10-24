#!/bin/bash
# ota host test — compiles the SHIPPED firmware/main/tiny/tiny_ota_util.h
# (board-stamp parser + media URL / sha256 guards), runs its cases, then
# checks tools/ota_push.py's Python twin reaches the same verdicts on the
# same fixture bytes. If a firmware build is present, its real .bin must
# carry the stamp of the board it was built for.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I ../../firmware/main ota_cases.cpp -o /tmp/ota_test
fx=$(mktemp -d)
trap 'rm -rf "$fx"' EXIT
/tmp/ota_test "$fx"

push=../../tools/ota_push.py
want() {  # want <file> <verdict> [board]
  out=$(python3 "$push" --inspect "$1" || true)
  v=$(python3 -c 'import json,sys; d=json.loads(sys.argv[1]); print(d["verdict"] + ":" + d["board"])' "$out")
  if [ "$v" = "$2:${3:-}" ]; then
    echo "PASS py $(basename "$1") -> $v"
  else
    echo "FAIL py $(basename "$1") -> '$v', want '$2:${3:-}'"; exit 1
  fi
}
want "$fx/own.bin" ok fnk0104s
want "$fx/foreign.bin" ok amoled18
want "$fx/unstamped.bin" unstamped
want "$fx/truncated.bin" truncated

# Pusher decisions: reply classification, offline configured board fails.
python3 ota_push_cases.py

for b in build:reterminal build-fnk0104s:fnk0104s build-amoled18:amoled18; do
  bin=../../firmware/${b%%:*}/tiny_sticky.bin
  [ -f "$bin" ] || continue
  # A build from before the stamp landed is not this tree's output: skip it.
  map="../../firmware/${b%%:*}/tiny_sticky.map"
  m=$(grep -c tiny_board_stamp "$map" 2>/dev/null || true)
  [ "${m:-0}" -gt 0 ] || continue
  want "$bin" ok "${b##*:}"
done
echo "ota host test passed"
