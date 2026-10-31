#!/bin/bash
# dictate host test — compiles the SHIPPED firmware/main/tiny/tiny_dictate_util.h
# (composer merge, rune-safe backspace, transcribe reply verdicts, heartbeat
# features[] probe) against the device's vendored cJSON (third_party/cjson).
set -euo pipefail
cd "$(dirname "$0")"
cc -std=c11 -O1 -w -I third_party/cjson -c third_party/cjson/cJSON.c -o /tmp/dictate_cjson.o
c++ -std=c++17 -Wall -Werror -I third_party/cjson -I ../../firmware/main \
    dictate_cases.cpp /tmp/dictate_cjson.o -o /tmp/dictate_test
/tmp/dictate_test
