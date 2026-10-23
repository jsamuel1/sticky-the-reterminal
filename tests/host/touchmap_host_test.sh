#!/bin/bash
# touchmap host test — compiles the SHIPPED firmware/main/tiny/tiny_touch_map.h
# (FT6336 parser + sensor->physical axis map) with the SHIPPED canvas.cpp and
# font.cpp, and checks the map composes with Canvas::to_logical.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I ../../firmware/main \
    ../../firmware/main/canvas.cpp ../../firmware/main/font.cpp \
    touchmap_cases.cpp -o /tmp/touchmap_test
/tmp/touchmap_test
echo "touchmap host test: ALL GREEN"
