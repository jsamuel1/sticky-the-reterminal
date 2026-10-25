#!/bin/bash
# lcd_canvas host test — links shipped lcd_canvas.cpp against a fake strip
# panel that fails after earlier asynchronous strips have reached panel RAM.
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror \
    -I lcd_canvas_shim -I shim -I ../../firmware/main \
    ../../firmware/main/lcd_canvas.cpp ../../firmware/main/canvas.cpp \
    ../../firmware/main/font.cpp lcd_canvas_cases.cpp \
    -o "$out/lcd_canvas_test"
"$out/lcd_canvas_test"
echo "lcd_canvas host test: ALL GREEN"
