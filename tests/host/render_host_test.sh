#!/bin/bash
# render host test — compiles the SHIPPED firmware/main/tiny_display.cpp with
# canvas.cpp, font.cpp and the device's own cJSON + qrcodegen (third_party/),
# renders every card type at each board's panel size, and checks edge clipping,
# tap-region bounds and the per-size golden frames (render_golden.txt).
#
#   RENDER_OUT=dir  also write a PNG per frame (PR / card evidence)
#   --update        rewrite render_golden.txt (only when a layout change is
#                   intended; the diff shows exactly which frames moved)
set -euo pipefail
cd "$(dirname "$0")"
OUT=/tmp/render_test
cc -std=c11 -O1 -w -I third_party/cjson -c third_party/cjson/cJSON.c -o /tmp/render_cjson.o
for f in qrcodegen esp_qrcode_main esp_qrcode_wrapper; do
    cc -std=c11 -O1 -w -I render_shim -I third_party/qrcodegen \
        -include stdlib.h -c third_party/qrcodegen/$f.c -o /tmp/render_$f.o
done
c++ -std=c++17 -O1 -Wall -Werror -Wno-unused-function -Wno-nonnull -pthread \
    -include render_shim/render_shim.h \
    -I render_shim -I third_party/cjson -I third_party/qrcodegen \
    -I ../../firmware/main \
    ../../firmware/main/tiny_display.cpp ../../firmware/main/canvas.cpp \
    ../../firmware/main/font.cpp render_cases.cpp \
    /tmp/render_cjson.o /tmp/render_qrcodegen.o /tmp/render_esp_qrcode_main.o \
    /tmp/render_esp_qrcode_wrapper.o -lz -o "$OUT"
if [ -n "${RENDER_OUT:-}" ]; then mkdir -p "$RENDER_OUT"; fi
"$OUT" render_golden.txt "$@"
echo "render host test: ALL GREEN"
