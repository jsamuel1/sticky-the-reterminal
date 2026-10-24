#!/bin/bash
# batt_curve host test — compiles the SHIPPED firmware/main/tiny_batt_curve.cpp
# (FNK0104S ADC battery: median, divider, LiPo curve, plausibility).
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I ../../firmware/main \
    ../../firmware/main/tiny_batt_curve.cpp batt_curve_cases.cpp -o /tmp/batt_curve_test
/tmp/batt_curve_test
echo "batt_curve host test: ALL GREEN"
