#!/bin/bash
# lock board contract host test — shared capability prose and startup evidence
# must distinguish chord-equipped reTerminal from no-chord colour boards.
set -euo pipefail
cd "$(dirname "$0")"
python3 - ../../firmware/main <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])

caps = (root / "tiny" / "board_caps.h").read_text()
line = next((x for x in caps.splitlines() if re.search(r"\bbool\s+imu\s*;", x)), "")
if "orientation" not in line or "board-specific" not in line or "pocket lock" in line:
    sys.exit(f"FAIL board_caps imu contract is not board-specific: {line!r}")
print("PASS board_caps imu contract keeps auto-lock board-specific")

sensors = (root / "tiny_sensors.cpp").read_text()
probe = sensors[sensors.index("// Gyroscope capability"):sensors.index("StickyGyroReading gyro")]
for required in ("board-selected", "~110 ms", "accelerometer-only", "NOT_SUPPORTED"):
    if required not in probe:
        sys.exit(f"FAIL gyro contract missing {required!r}")
print("PASS shared gyro contract covers supported and accel-only boards")

lock = (root / "tiny_lock.cpp").read_text()
init = lock[lock.index('extern "C" esp_err_t tiny_lock_init'):]
required = (
    "#if PIN_BTN_UP < 0 || PIN_BTN_DOWN < 0",
    "chord=none",
    "IMU auto-entry disabled",
    "chord=UP+DOWN",
    "entry=manual+IMU",
)
for token in required:
    if token not in init:
        sys.exit(f"FAIL lock startup evidence missing {token!r}")
if "IMU auto-entry is the next package" in init:
    sys.exit("FAIL lock startup evidence retains stale future-package claim")
print("PASS lock startup evidence distinguishes chord/no-chord boards")

header = (root / "tiny" / "tiny_lock.h").read_text()
top = header[:header.index("#pragma once")]
for required in ("boards with an UP+DOWN", "Boards without", "`unlock`"):
    if required not in top:
        sys.exit(f"FAIL tiny_lock.h contract missing {required!r}")
print("PASS tiny_lock.h entry/exit contract is board-specific")

main = (root / "tiny_main.cpp").read_text()
startup = main[main.index("// §11 pocket lockout: armed"):main.index('ESP_LOGI(TAG, "lockout init:')]
for required in ("Boards with both", "allow IMU auto-entry", "no-chord boards", "manual relay lock/unlock"):
    if required not in startup:
        sys.exit(f"FAIL tiny_main lock startup contract missing {required!r}")
print("PASS tiny_main startup contract is board-specific")
print("lock board contract host test: ALL GREEN")
PY
