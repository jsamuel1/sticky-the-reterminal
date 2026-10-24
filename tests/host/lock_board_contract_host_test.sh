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

# Owner-facing unlock guidance: one board-aware helper, both branches pinned.
hint = lock[lock.index('extern "C" const char *tiny_lock_unlock_hint'):]
hint = hint[:hint.index("\n}\n")]
no_chord, _, chord = hint.partition("#else")
if "#if PIN_BTN_UP < 0 || PIN_BTN_DOWN < 0" not in no_chord:
    sys.exit("FAIL unlock hint is not selected by the UP/DOWN pins")
if "send unlock" not in no_chord or re.search(r"UP\s*\+\s*DOWN", no_chord, re.I):
    sys.exit(f"FAIL no-chord unlock hint must name only the relay: {no_chord!r}")
if "hold UP+DOWN 1s" not in chord or "send unlock" not in chord:
    sys.exit(f"FAIL chord unlock hint must name chord and relay: {chord!r}")
print("PASS unlock hint names chord only where both keys exist")

# No runtime string may hard-code the chord unless it sits inside a
# reTerminal/chord-only preprocessor branch. Comments are stripped first so
# prose about the chord stays allowed; string literals are what owners see.
def strip_comments(src):
    out, i, n = [], 0, len(src)
    while i < n:
        if src.startswith("//", i):
            j = src.find("\n", i); i = n if j < 0 else j
        elif src.startswith("/*", i):
            j = src.find("*/", i + 2); i = n if j < 0 else j + 2
        elif src[i] == "'":
            j = i + 1
            while j < n and src[j] != "'":
                j += 2 if src[j] == "\\" else 1
            i = j + 1
        elif src[i] == '"':
            j = i + 1
            while j < n and src[j] != '"':
                j += 2 if src[j] == "\\" else 1
            out.append(src[i:j + 1]); i = j + 1
        else:
            out.append(src[i]); i += 1
    return "".join(out)

CHORD = re.compile(r"UP\s*\+\s*DOWN|up\s*\+\s*down")
CHORD_ONLY = re.compile(r"#if\s+(defined\(CONFIG_TINY_BOARD_RETERMINAL\)|PIN_BTN_UP\s*>=\s*0)")
NO_CHORD = re.compile(r"#if\s+(!\s*defined\(CONFIG_TINY_BOARD_RETERMINAL\)|PIN_BTN_UP\s*<\s*0)")
bad = []
for path in sorted(root.rglob("*.cpp")):
    stack = []  # True = inside a chord-only branch
    for no, raw in enumerate(strip_comments(path.read_text()).splitlines(), 1):
        s = raw.strip()
        if CHORD_ONLY.match(s):   stack.append(True);  continue
        if NO_CHORD.match(s):     stack.append(False); continue
        if s.startswith("#if"):   stack.append(None);  continue
        if s.startswith("#else") and stack:
            if stack[-1] is not None: stack[-1] = not stack[-1]
            continue
        if s.startswith("#endif") and stack: stack.pop(); continue
        if re.search(r'"[^"]*(UP\s*\+\s*DOWN|up\s*\+\s*down)[^"]*"', raw) and True not in stack:
            bad.append(f"{path.relative_to(root)}:{no}: {s}")
if bad:
    sys.exit("FAIL universal UP+DOWN guidance in runtime strings:\n  " + "\n  ".join(bad))
print("PASS no runtime string promises UP+DOWN outside chord-only branches")

node = (root / "tiny_node.cpp").read_text()
if node.count("tiny_lock_unlock_hint()") < 4:
    sys.exit("FAIL lock/voice/mic/sleep replies must take tiny_lock_unlock_hint()")
print("PASS lock, voice, mic and sleep replies use the board-aware hint")

onboard = (root / "tiny_onboard.cpp").read_text()
if onboard.count("#if defined(CONFIG_TINY_BOARD_RETERMINAL)") < 2 or "send unlock" not in onboard:
    sys.exit("FAIL onboarding tour must branch per board and teach relay unlock on no-chord boards")
print("PASS onboarding copy branches per board")

nodeh = (root / "tiny" / "tiny_node.h").read_text()
autosleep = node[node.index("// A LOCKED session is not a session."):node.index("static bool autosleep_due")]
for name, text in (("tiny_node.cpp autosleep", autosleep),
                   ("tiny_lock.cpp chord_task", lock[lock.index("// Unlock-chord watcher"):lock.index("static void chord_task")])):
    if re.search(r"ONLY path back|nothing a human can do except", text):
        sys.exit(f"FAIL {name} comment calls the chord the sole exit")
    if not re.search(r"relay|no-chord", text):
        sys.exit(f"FAIL {name} comment omits the no-chord relay exit")
print("PASS chord-as-sole-exit commentary is board-scoped")

# Public docs: the chord and IMU auto-lock are reTerminal-only.
docs = root.parent.parent / "docs"
sticky = (docs / "stickyos.md").read_text()
row = next(x for x in sticky.splitlines() if x.startswith("| **UP+DOWN held 1 s**"))
if "reTerminal only" not in row:
    sys.exit(f"FAIL stickyos chord row is not scoped: {row!r}")
pocket = sticky[sticky.index("**The pocket lock.**"):sticky.index("**The title bar**")]
for required in ("On every board", "On the reTerminal I also lock myself", "never lock themselves from the IMU"):
    if required not in pocket:
        sys.exit(f"FAIL stickyos pocket lock missing {required!r}")
glass = (docs / "dashboard" / "frontend" / "src" / "components" / "Glass.tsx").read_text()
if "on-device chord: UP+DOWN 1s)" in glass or "colour boards" not in glass:
    sys.exit("FAIL dashboard unlock title/comment presents the chord as universal")
print("PASS public docs scope chord and IMU auto-lock to the reTerminal")
print("lock board contract host test: ALL GREEN")
PY
