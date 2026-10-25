#!/bin/bash
# agent roster host test — compiles the SHIPPED firmware/main/tiny_agent.cpp
# against the file-backed NVS shim and checks that the roster PREVIEW a
# universe push paints from (tiny_agent_roster_preview_add) is exactly what
# the real add would persist, and writes nothing itself. See agent_cases.cpp.
set -euo pipefail
cd "$(dirname "$0")"
export ASKQ_NVS_FILE=/tmp/agent_nvs_store.tsv
rm -f "$ASKQ_NVS_FILE"
c++ -std=c++17 -Wall -Werror -include rotation_shim/rotation_shim.h \
    -I shim -I rotation_shim -I ../../firmware/main \
    ../../firmware/main/tiny_agent.cpp agent_cases.cpp -o /tmp/agent_test
/tmp/agent_test
rm -f "$ASKQ_NVS_FILE"
echo "agent host test: ALL GREEN"
