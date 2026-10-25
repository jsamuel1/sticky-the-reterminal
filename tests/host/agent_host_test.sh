#!/bin/bash
# agent roster host test — compiles the SHIPPED firmware/main/tiny_agent.cpp
# against the file-backed NVS shim plus save-path fault injection
# (agent_shim/agent_shim.h). Each run of the binary is one boot; runs that
# share a store file are a reboot. See agent_cases.cpp for the phases.
set -euo pipefail
cd "$(dirname "$0")"
export ASKQ_NVS_FILE=/tmp/agent_nvs_store.tsv
export AGENT_EXPECT_FILE=/tmp/agent_expect.txt
rm -f "$ASKQ_NVS_FILE" "$AGENT_EXPECT_FILE"
c++ -std=c++17 -Wall -Werror -include agent_shim/agent_shim.h \
    -I shim -I agent_shim -I ../../firmware/main \
    ../../firmware/main/tiny_agent.cpp agent_cases.cpp -o /tmp/agent_test
/tmp/agent_test fresh
/tmp/agent_test reboot
# A store written by pre-blob firmware: one key per field.
printf 'tinyagent.slug\tbonk\ntinyagent.rcount\t3\ntinyagent.r0\ttiny\ntinyagent.r1\tbonk\ntinyagent.r2\tclod\n' \
    > "$ASKQ_NVS_FILE"
/tmp/agent_test legacy
/tmp/agent_test legacy-reboot
# An unreadable "state" blob (wrong length) beside legacy keys.
printf 'tinyagent.state\tx00010203\ntinyagent.rcount\t2\ntinyagent.r0\ttiny\ntinyagent.r1\tbonk\n' \
    > "$ASKQ_NVS_FILE"
/tmp/agent_test corrupt
/tmp/agent_test corrupt-reboot
# Outcome-unknown writes: each scenario in its own store, then a reboot.
for s in $(/tmp/agent_test outcomes); do
    rm -f "$ASKQ_NVS_FILE" "$AGENT_EXPECT_FILE"
    /tmp/agent_test outcome "$s"
    /tmp/agent_test outcome-reboot "$s"
done
rm -f "$ASKQ_NVS_FILE" "$AGENT_EXPECT_FILE"
echo "agent host test: ALL GREEN"
