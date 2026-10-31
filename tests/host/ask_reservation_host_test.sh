#!/bin/bash
# ask reservation host test — compiles the shipped atomic slot.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -pthread -I ../../firmware/main \
    ask_reservation_cases.cpp -o /tmp/ask_reservation_test
/tmp/ask_reservation_test
