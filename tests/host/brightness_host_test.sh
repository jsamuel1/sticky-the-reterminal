#!/bin/bash
# brightness host test — compiles the SHIPPED firmware/main/tiny/tiny_brightness.h
# (the `brightness` verb's argument parse, the boot restore level and the
# settings row cycle, grammar v22).
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I ../../firmware/main brightness_cases.cpp -o /tmp/brightness_test
/tmp/brightness_test
echo "brightness host test: ALL GREEN"
