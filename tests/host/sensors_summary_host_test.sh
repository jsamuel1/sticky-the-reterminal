#!/bin/bash
# sensors summary host test — the `sensors` reply's human `summary` sentence
# counts the entries of errors[]. errors[] holds faults (a missing part, a
# failed read, an FNK divider with no plausible cell, an untrusted gauge), so
# the count must be labelled "errors". It was "(%d unread)", which turned
# "adc: no plausible cell (180 mV)" on a USB-only FNK0104S into "(1 unread)".
# The glass footer must also name the divider when the ADC read fails, not
# fall through to the reTerminal's "read live over I2C1", and must say "no
# plausible cell" only for ESP_ERR_INVALID_RESPONSE (other ADC faults: "no
# reading").
set -euo pipefail
cd "$(dirname "$0")"
python3 - ../../firmware/main/tiny_sensors.cpp <<'PY'
import pathlib, re, sys
text = pathlib.Path(sys.argv[1]).read_text()
code = re.sub(r"//[^\n]*", "", text)  # comments may quote the old label

count = re.findall(
    r'append\(\s*"([^"]*)"\s*,\s*cJSON_GetArraySize\(\s*errors\s*\)\s*\)', code)
if len(count) != 1:
    sys.exit(f"FAIL expected one summary append of the errors[] count, found {len(count)}")
fmt = count[0]
if not re.fullmatch(r" \(%d errors\)", fmt):
    sys.exit(f"FAIL summary labels errors[] count as {fmt!r}, want ' (%d errors)'")
print(f"PASS summary errors[] count format {fmt!r}")

if re.search(r"%d\s+unread", code):
    sys.exit("FAIL tiny_sensors.cpp still formats a count as 'unread'")
print("PASS no '%d unread' label in tiny_sensors.cpp")

foot = re.search(r"char\s+foot\s*\[[^\]]*\];(.*?)AddStringToObject\(\s*c\s*,\s*\"footer\"",
                 code, re.S)
if not foot:
    sys.exit("FAIL sensors card footer block not found")
body = foot.group(1)
first = re.search(r"if\s*\(([^\n]*)\)", body)
if not first or "have_bat" in first.group(1) or '"adc"' not in first.group(1):
    got = first.group(1) if first else "none"
    sys.exit(f"FAIL footer's first branch must test bat.source == adc alone, got {got!r}")
if "no plausible cell" not in body:
    sys.exit("FAIL footer has no ADC-failure wording ('no plausible cell')")
print("PASS footer names the GPIO 9 divider on ADC failure")

# "no plausible cell" is a claim about the hardware: only
# ESP_ERR_INVALID_RESPONSE (measured, not a resting cell) earns it. An ADC
# init/read/calibration fault must say "no reading", so the footer has to
# test the exact error, not have_bat alone.
render = code[code.index("tiny_sensors_render_card"):]
if not re.search(r"esp_err_t\s+be\s*=\s*sticky_battery_read_detail\(\s*bat\s*\)", render):
    sys.exit("FAIL render_card drops sticky_battery_read_detail's error code")
cell = re.search(r'be\s*==\s*ESP_ERR_INVALID_RESPONSE\s*\?\s*"no plausible cell"\s*:\s*"no reading"', body)
if not cell:
    sys.exit("FAIL footer must say 'no plausible cell' only for ESP_ERR_INVALID_RESPONSE, else 'no reading'")
print("PASS footer reserves 'no plausible cell' for ESP_ERR_INVALID_RESPONSE")
print("sensors summary host test passed")
PY
