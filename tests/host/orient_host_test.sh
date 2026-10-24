#!/bin/bash
# orient host test — compiles the shipped per-board orientation tables
# (tiny/tiny_orient_map.h) with the shipped Canvas and checks each IMU axis
# turns the picture so its top sits on the edge that is physically up.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I ../../firmware/main \
    ../../firmware/main/canvas.cpp ../../firmware/main/font.cpp \
    orient_cases.cpp -o /tmp/orient_test
/tmp/orient_test
echo "orient host test: ALL GREEN"
