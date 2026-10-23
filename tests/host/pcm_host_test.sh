#!/bin/bash
# pcm host test — compiles the SHIPPED firmware/main/tiny_pcm.cpp (WAV header,
# chime notes, RTTTL, tone renderer, stats used by the ES8311 boards).
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -I ../../firmware/main \
    ../../firmware/main/tiny_pcm.cpp pcm_cases.cpp -o /tmp/pcm_test -lm
/tmp/pcm_test
echo "pcm host test: ALL GREEN"
