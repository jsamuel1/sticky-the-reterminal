#!/bin/bash
# envelope host test — compiles the SHIPPED firmware/main/tiny_envelope.cpp
# with the device's vendored cJSON (third_party/cjson): the log line for a
# relay envelope without a prompt names its shape, and credential values never
# reach it.
set -euo pipefail
cd "$(dirname "$0")"
cc -std=c11 -O1 -w -I third_party/cjson -c third_party/cjson/cJSON.c -o /tmp/envelope_cjson.o
c++ -std=c++17 -Wall -Werror -I third_party/cjson -I ../../firmware/main \
    ../../firmware/main/tiny_envelope.cpp envelope_cases.cpp /tmp/envelope_cjson.o \
    -o /tmp/envelope_test
/tmp/envelope_test
