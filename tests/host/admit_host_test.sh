#!/bin/bash
# admit host test — compiles the SHIPPED firmware/main/tiny/tiny_admit.h (the
# work/sleep admission gate) and races it with real threads.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -O1 -pthread -I ../../firmware/main admit_cases.cpp -o /tmp/admit_test
/tmp/admit_test
echo "admit host test: ALL GREEN"
