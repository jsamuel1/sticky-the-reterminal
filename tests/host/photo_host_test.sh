#!/bin/bash
# photo host test — compiles the SHIPPED firmware/main/tiny/tiny_photo_util.h
# (fit, TJpgDec descale choice, area resample, error-diffusion dither) and
# runs its cases. The render host test covers drawing through the same header.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I ../../firmware/main photo_cases.cpp -o /tmp/photo_test
/tmp/photo_test
echo "photo host test passed"
