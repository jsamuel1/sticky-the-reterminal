#!/bin/bash
# touchmap host test — compiles shipped colour-board touch parsers/maps with
# shipped Canvas and verifies physical-to-logical composition.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I ../../firmware/main \
    ../../firmware/main/canvas.cpp ../../firmware/main/font.cpp \
    touchmap_cases.cpp -o /tmp/touchmap_test
/tmp/touchmap_test
echo "touchmap host test: ALL GREEN"
