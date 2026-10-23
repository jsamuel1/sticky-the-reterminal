#!/bin/bash
# pair host test — compiles the SHIPPED firmware/main/tiny/tiny_pair_util.h
# (sha256, base64url, secretHash, code/url, merge body, poll classifier).
# Golden vectors are Node's, the runtime tiny-tech enroll.js hashes with.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I ../../firmware/main pair_cases.cpp -o /tmp/pair_test
/tmp/pair_test
