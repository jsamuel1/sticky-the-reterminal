#!/bin/bash
# batt_curve host test — compiles the SHIPPED firmware/main/tiny_batt_curve.cpp
# (FNK0104S ADC battery: median, divider, LiPo curve, plausibility).
set -euo pipefail
cd "$(dirname "$0")"
# UBSan in trap mode (no runtime library needed): a signed overflow kills the
# binary instead of passing because the wrapped value happened to look right.
c++ -std=c++17 -Wall -Werror -fsanitize=undefined -fsanitize-undefined-trap-on-error \
    -I ../../firmware/main \
    ../../firmware/main/tiny_batt_curve.cpp batt_curve_cases.cpp -o /tmp/batt_curve_test
/tmp/batt_curve_test
echo "batt_curve host test: ALL GREEN"
