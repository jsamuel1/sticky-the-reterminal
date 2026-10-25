#!/bin/bash
# rotation host test — manual display rotation for boards WITHOUT an
# accelerometer (grammar v25).
#
# 1. Compiles the SHIPPED tiny/tiny_rotation.h + tiny_config.cpp (file-backed
#    NVS shim: each process run below is one boot) and checks the rules,
#    persistence across reboot, the unset/invalid default, and that a failed
#    save never claims to be persisted.
# 2. Source contract on the IDF-only glue that cannot run on a host:
#    board_caps says which board lacks an IMU; boot restores the choice only
#    there; settings shows the picker only there and the auto/lock row only on
#    IMU boards; taps route; tiny_orient (gravity) is untouched by this path.
# The picker's on-glass rendering, tap/gesture alignment at every angle and
# reboot preset are in render_host_test.sh (render_cases.cpp, "manual rotation").
set -euo pipefail
cd "$(dirname "$0")"
export ASKQ_NVS_FILE=/tmp/rotation_nvs_store.tsv
rm -f "$ASKQ_NVS_FILE"
cc -std=c11 -O1 -w -I third_party/cjson -c third_party/cjson/cJSON.c -o /tmp/rotation_cjson.o
c++ -std=c++17 -Wall -Werror -include rotation_shim/rotation_shim.h \
    -I shim -I rotation_shim -I third_party/cjson -I ../../firmware/main \
    ../../firmware/main/tiny_config.cpp rotation_cases.cpp /tmp/rotation_cjson.o \
    -o /tmp/rotation_test
T=/tmp/rotation_test
$T rules
$T fresh                       # never written -> 0 deg
for d in 90 180 270 0; do
    $T save $d                 # pick it from settings
    $T boot $d                 # reboot: it comes back
done
$T save 270
$T failsave                    # NVS write fails: stored 270 survives
$T boot 270
$T raw 7                       # a value no firmware offers -> default
$T boot 0
$T raw 255
$T boot 0
$T raw 2
$T boot 180
rm -f "$ASKQ_NVS_FILE"

# ---- source contract (glue that only exists on the device) ----
python3 - ../../firmware/main <<'PY'
import pathlib, re, sys
root = pathlib.Path(sys.argv[1])
fails = []
def need(cond, msg):
    if not cond: fails.append(msg)

# board_caps: exactly the boards the vendor/schematic evidence says.
caps = (root / "board_caps.cpp").read_text()
blocks = re.split(r"#(?:el)?if defined\(CONFIG_TINY_BOARD_(\w+)\)", caps)
imu = {}
for i in range(1, len(blocks) - 1, 2):
    m = re.search(r"\.imu\s*=\s*(true|false)", blocks[i + 1])
    if m: imu[blocks[i]] = m.group(1) == "true"
need(imu == {"RETERMINAL": True, "FNK0104S": False, "AMOLED18": True},
     f"board_caps imu table changed: {imu} (manual rotation gates on it)")

main = (root / "tiny_main.cpp").read_text()
m = re.search(r"if \(tiny_rotation_manual_offered\(tiny_board_caps\(\)->imu\)\) \{(.*?)\n    \}", main, re.S)
need(m is not None, "tiny_main: boot restore not gated on tiny_rotation_manual_offered(caps->imu)")
if m:
    body = m.group(1)
    need("tiny_config_rotation_get" in body and "tiny_rotation_boot_deg" in body
         and "tiny_display_preset_rotation" in body,
         "tiny_main: boot restore must read config -> boot_deg -> preset")
    need(main.index("tiny_display_preset_rotation") < main.index("tiny_display_splash("),
         "tiny_main: saved rotation must be seeded before the splash/first card")
need(main.count("tiny_display_preset_rotation") == 1,
     "tiny_main: preset_rotation called outside the non-IMU gate")

shell = (root / "tiny_shell.cpp").read_text()
m = re.search(r"if \(tiny_board_caps\(\)->imu\) \{(.*?)\} else if \(tiny_rotation_manual_offered\(tiny_board_caps\(\)->imu\)\) \{(.*?)\n        \}", shell, re.S)
need(m is not None, "settings: rotlock (IMU) / rot:open (no IMU) rows must be exclusive branches")
if m:
    need('"rotlock"' in m.group(1) and '"rot:open"' not in m.group(1),
         "settings: IMU branch must keep the auto/lock row only")
    need('"rot:open"' in m.group(2) and '"rotlock"' not in m.group(2),
         "settings: no-IMU branch must offer the picker only")
r = re.search(r"static esp_err_t render_rotation\(void\) \{(.*?)\n\}", shell, re.S)
need(r and "tiny_rotation_manual_offered(tiny_board_caps()->imu)" in r.group(1)
     and "ESP_ERR_NOT_SUPPORTED" in r.group(1),
     "render_rotation must refuse on an IMU board")
rr = re.search(r"static esp_err_t rotation_route\(const char \*id\) \{(.*?)\n\}", shell, re.S)
need(rr and "tiny_rotation_select(" in rr.group(1) and "tiny_board_caps()->imu" in rr.group(1),
     "rotation_route must decide through tiny_rotation_select with caps->imu")
need(re.search(r"rotation_apply\(int deg\) \{ return tiny_display_set_rotation\(deg\); \}", shell),
     "picker must apply through tiny_display_set_rotation (one rotation path)")
need(re.search(r"rotation_save\(uint8_t q\) \{ return tiny_config_rotation_set\(q\); \}", shell),
     "picker must save through tiny_config_rotation_set")
need(re.search(r'if \(!strncmp\(id, "rot:", 4\)\) return rotation_route\(id\);', shell),
     "tiny_shell_menu must route rot:* ids")
for bad in ("tiny_orient_set_auto", "tiny_config_rotauto"):
    need(not (rr and bad in rr.group(1)), f"rotation_route must not touch {bad}")

touch = (root / "tiny_touch.cpp").read_text()
fn = re.search(r"Act action_for\(.*?\n\}", touch, re.S).group(0)
need('!strncmp(id, "rot:", 4)) return Act::kMenu' in fn, "touch: rot:* must route to kMenu")
need(fn.index('"rot:", 4') < fn.index("token_match(s, \"back\")"),
     "touch: rot:* must be matched above the generic back token")

orient = (root / "tiny_orient.cpp").read_text()
need("tiny_rotation" not in orient and "rotq" not in orient,
     "tiny_orient (gravity path) must not read the manual choice")
need(re.search(r"if \(on && !tiny_board_caps\(\)->imu\)", orient),
     "tiny_orient: auto still refused on boards without an IMU")
need("tiny_orient_start()" in main and "tiny_board_caps()->imu ? tiny_orient_start()" in main,
     "tiny_main: gravity watcher still starts only on IMU boards")

if fails:
    for f in fails: print("FAIL", f)
    sys.exit(1)
print("rotation source contract: green")
PY
echo "rotation host test: ALL GREEN"
