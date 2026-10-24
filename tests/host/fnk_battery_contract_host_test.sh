#!/bin/bash
# FNK battery contract test — developer/config guidance must match shipped
# grammar and the GPIO 9 ADC implementation.
set -euo pipefail
cd "$(dirname "$0")"
python3 - ../../firmware/main/tiny/tiny_version.h ../../firmware/main/Kconfig.projbuild ../../AGENTS.md ../../firmware/main/tiny/tiny_sensors.h ../../firmware/main/tiny_display.cpp ../../firmware/main/tiny_main.cpp <<'PY'
import pathlib
import re
import sys

version_h, kconfig_p, agents_p, sensors_h_p, display_p, main_p = map(pathlib.Path, sys.argv[1:])
version_text = version_h.read_text()
kconfig = kconfig_p.read_text()
agents = agents_p.read_text()
sensors_h = sensors_h_p.read_text()
display = display_p.read_text()
main = main_p.read_text()

version_match = re.search(r"^#define\s+TINY_GRAMMAR_VERSION\s+(\d+)\b", version_text, re.M)
if not version_match:
    sys.exit("FAIL TINY_GRAMMAR_VERSION not found")
version = int(version_match.group(1))

current_match = re.search(r"\(int, currently (\d+)\)", agents)
heading_match = re.search(r"^## 5\. The verb grammar \(v(\d+)\)", agents, re.M)
if not current_match or not heading_match:
    sys.exit("FAIL AGENTS grammar markers missing")
if {int(current_match.group(1)), int(heading_match.group(1))} != {version}:
    sys.exit(
        f"FAIL AGENTS grammar markers {current_match.group(1)}/{heading_match.group(1)} "
        f"do not match TINY_GRAMMAR_VERSION {version}"
    )
print(f"PASS AGENTS grammar markers match v{version}")

fnk = re.search(
    r"config TINY_BOARD_FNK0104S\b(.*?)(?=\nconfig TINY_BOARD_|\nendchoice)",
    kconfig,
    re.S,
)
if not fnk:
    sys.exit("FAIL FNK0104S Kconfig block missing")
fnk_help = fnk.group(1)
for required in ("GPIO 9 battery ADC", "resting-voltage percent estimate"):
    if required not in fnk_help:
        sys.exit(f"FAIL FNK0104S Kconfig help missing {required!r}")
if re.search(r"battery ADC[^\n]*not driven|battery ADC and SD are not driven", fnk_help):
    sys.exit("FAIL FNK0104S Kconfig still calls battery ADC absent")
print("PASS FNK0104S Kconfig advertises driven ADC estimate")

sensor_row = re.search(r"^\| `tiny_sensors` \|(.+)$", agents, re.M)
if not sensor_row:
    sys.exit("FAIL AGENTS tiny_sensors module row missing")
for required in ("GPIO 9 ADC", "cached 1 s", "nulls never fabricated"):
    if required not in sensor_row.group(1):
        sys.exit(f"FAIL AGENTS tiny_sensors row missing {required!r}")
print("PASS AGENTS names ADC source and one-second cache")

for required in ("BQ27220 gauge", "GPIO 9 ADC divider", "shares one judged", "1 s"):
    if required not in sensors_h:
        sys.exit(f"FAIL tiny_sensors.h missing {required!r}")
if "never a stale cache" in sensors_h:
    sys.exit("FAIL tiny_sensors.h still denies intentional ADC cache")
print("PASS tiny_sensors.h states fresh-vs-cached contract")

if "battery only when its active source answers" not in display:
    sys.exit("FAIL tiny_display.cpp still describes battery as gauge-only")
print("PASS title-bar contract permits gauge or ADC source")

for required in ("battery source (BQ27220 gauge or", "FNK GPIO 9 ADC)",
                 "local reads — I2C or ADC"):
    if required not in main:
        sys.exit(f"FAIL tiny_main.cpp boot contract missing {required!r}")
if "Both are\n    // millisecond I2C reads" in main:
    sys.exit("FAIL tiny_main.cpp still calls both boot reads I2C")
print("PASS boot contract names board-specific battery transport")
PY
echo "FNK battery contract host test: ALL GREEN"
