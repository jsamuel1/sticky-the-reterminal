#!/bin/bash
# pmu host test — compiles the SHIPPED firmware/main/tiny/tiny_pmu_util.h
# (AXP2101 status/ADC/PWR-key decode, PCF85063A BCD) and checks it against
# register values worked out from the vendor's XPowersLib and SensorLib.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I ../../firmware/main pmu_cases.cpp -o /tmp/pmu_test
/tmp/pmu_test
echo "pmu host test: ALL GREEN"
