#!/bin/bash
# led host test — compiles the SHIPPED firmware/main/tiny/tiny_led_state.h
# (FNK0104S status light: which colour for which state, lock/sleep dark,
# error expiry, brightness scaling).
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I ../../firmware/main led_cases.cpp -o /tmp/led_test
/tmp/led_test
echo "led host test: ALL GREEN"
