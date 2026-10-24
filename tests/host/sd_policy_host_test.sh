#!/bin/bash
# sd policy host test — compiles the SHIPPED firmware/main/tiny/tiny_sd_policy.h
# (the SDMMC microSD driver's probe rules: error classification, 1-bit retry,
# widths, empty-slot backoff) against the host esp_err shim.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I shim -I ../../firmware/main/tiny \
    sd_policy_cases.cpp -o /tmp/sd_policy_test
/tmp/sd_policy_test
echo "sd policy host test: ALL GREEN"
