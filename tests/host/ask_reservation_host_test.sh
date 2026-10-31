#!/bin/bash
# ask reservation host test — compiles the shipped atomic slot into TWO
# translation units and checks they share one slot (cross-TU linkage).
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -pthread -I ../../firmware/main \
    ask_reservation_cases.cpp ask_reservation_other_tu.cpp -o /tmp/ask_reservation_test
/tmp/ask_reservation_test
