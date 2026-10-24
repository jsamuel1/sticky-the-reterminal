#!/bin/bash
# provision host test — tools/provision_new.sh + tools/provision_lib.py, no board.
#  1. make_ssid() is EXTRACTED from the shipped tiny_provision.cpp and compiled
#     against an esp_read_mac shim that derives the softAP MAC the way IDF v5.4
#     does (base + 1, last octet). Its name must equal provision_lib's, for the
#     owner's FNK vector (base b8:1f:3f:c5:05:2c → tiny-eba6, not tiny-ed39).
#  2. provision_cases.py: flash plan from flasher_args.json, board/size guards,
#     the NVS blob, session + pairing enrolment against a mock tiny API.
#  3. provision_new.sh --dry-run end to end with --mac (no port, no network).
set -euo pipefail
cd "$(dirname "$0")"
ROOT="$(cd ../.. && pwd)"
LIB="$ROOT/tools/provision_lib.py"
GEN=/tmp/provision_ssid_gen.cpp

python3 - "$ROOT/firmware/main/tiny_provision.cpp" "$GEN" <<'PY'
import sys
src = open(sys.argv[1]).read()
i = src.index("static void make_ssid(void) {")
j = src.index("{", i); d = 0; k = j
while True:
    d += {"{": 1, "}": -1}.get(src[k], 0)
    if d == 0: break
    k += 1
open(sys.argv[2], "w").write(r'''#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
enum { ESP_MAC_WIFI_STA = 0, ESP_MAC_WIFI_SOFTAP = 1 };
static uint8_t g_base[6];
static int esp_read_mac(uint8_t *mac, int type) {   // IDF v5.4 mac_addr.c, UNIVERSE_WIFI_AP
    memcpy(mac, g_base, 6);
    if (type == ESP_MAC_WIFI_SOFTAP) mac[5] += 1;
    return 0;
}
static char s_ssid[16];
''' + src[i:k+1] + r'''
int main(int argc, char **argv) {
    for (int i = 0; i < 6; ++i) g_base[i] = (uint8_t)strtoul(argv[1] + 3 * i, nullptr, 16);
    make_ssid();
    puts(s_ssid);
}
''')
PY
c++ -std=c++17 -Wall -Werror -o /tmp/provision_ssid_test "$GEN"

for base in b8:1f:3f:c5:05:2c b8:1f:3f:d7:23:60 b8:1f:3f:d7:80:ff 00:00:00:00:00:00; do
  fw="$(/tmp/provision_ssid_test "$base")"
  py="$(python3 "$LIB" name "$base" | awk '{print $2}')"
  [ "$fw" = "$py" ] || { echo "FAIL: $base firmware=$fw provision_lib=$py"; exit 1; }
  echo "  $base → $fw (firmware == provision_lib)"
done
[ "$(/tmp/provision_ssid_test b8:1f:3f:c5:05:2c)" = tiny-eba6 ] || { echo "FAIL: FNK vector"; exit 1; }

python3 provision_cases.py

# the claimed roster: provision_lib's parse of tiny_node.cpp == the s_caps[] the
# firmware compiles (extracted, built, printed), so the parser cannot drift either
python3 - "$ROOT/firmware/main/tiny_node.cpp" /tmp/provision_caps_gen.cpp <<'PY'
import re, sys
src = open(sys.argv[1]).read()
m = re.search(r"static const char \*const s_caps\[\] = \{.*?\};", src, re.S)
open(sys.argv[2], "w").write("#include <cstdio>\n" + m.group(0) +
    "\nint main() { for (const char *c : s_caps) puts(c); }\n")
PY
c++ -std=c++17 -Wall -Werror -o /tmp/provision_caps_test /tmp/provision_caps_gen.cpp
fw_caps="$(/tmp/provision_caps_test | tr '\n' ' ')"
py_caps="$(python3 -c 'import sys; sys.path.insert(0, sys.argv[1]); import provision_lib as p; print(" ".join(p.firmware_caps(sys.argv[2])) + " ")' "$ROOT/tools" "$ROOT/firmware")"
[ "$fw_caps" = "$py_caps" ] || { echo "FAIL: caps firmware=[$fw_caps] provision_lib=[$py_caps]"; exit 1; }
echo "  caps: provision_lib == compiled s_caps[] ($(wc -w <<<"$fw_caps" | tr -d ' ') verbs)"

# dry run, no board: plan + routes, nothing written
out="$(env -u WIFI_SSID -u TINY_TOKEN TINY_HOME=/nonexistent TINY_API=http://127.0.0.1:9 \
       bash "$ROOT/tools/provision_new.sh" --board fnk0104s --dry-run --no-wifi --mac b8:1f:3f:c5:05:2c 2>&1)" \
  || { echo "$out"; echo "FAIL: dry run exited non-zero"; exit 1; }
for want in "name tiny-eba6" "board fnk0104s" "0x009000" "pairing code" "dry run"; do
  grep -q -- "$want" <<<"$out" || { echo "$out"; echo "FAIL: dry run lacks '$want'"; exit 1; }
done
echo "  dry run: tiny-eba6, NVS 0x9000, pairing route"

# a real run on uncommitted compiled input OUTSIDE firmware/main (partition
# table, vendor driver) refuses before it builds, enrols or writes. Fake esptool
# answers flash_id for the FNK; a plain file stands in for the port.
T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
mkdir -p "$T/py/esptool" "$T/repo/tools" "$T/repo/firmware/main" "$T/repo/vendor/Sticky_dashboard_demo/components/x"
printf 'import sys\nprint("esptool " + " ".join(sys.argv[1:]), file=open(sys.argv[0] + ".calls", "a"))\nprint("MAC: b8:1f:3f:c5:05:2c\\nDetected flash size: 16MB")\n' >"$T/py/esptool/__main__.py"
: >"$T/py/esptool/__init__.py"; : >"$T/py/serial.py"
printf '#!/bin/sh\nPYTHONPATH=%s exec python3 "$@"\n' "$T/py" >"$T/fakepy"; chmod +x "$T/fakepy"
cp "$ROOT/tools/provision_new.sh" "$ROOT/tools/provision_lib.py" "$ROOT/tools/idf.sh" "$ROOT/tools/build_board.sh" "$T/repo/tools/"
cp "$ROOT"/firmware/sdkconfig.defaults* "$ROOT"/firmware/partitions*.csv "$T/repo/firmware/"
cp "$ROOT/firmware/main/tiny_node.cpp" "$T/repo/firmware/main/"
printf 'drv\n' >"$T/repo/vendor/Sticky_dashboard_demo/components/x/x.c"
git -C "$T/repo" init -q && git -C "$T/repo" add -A
git -C "$T/repo" -c user.email=t@t -c user.name=t commit -qm init
: >"$T/port"
for dirt in firmware/partitions_16mb.csv vendor/Sticky_dashboard_demo/components/x/x.c; do
  git -C "$T/repo" checkout -q -- .
  printf '# edit\n' >>"$T/repo/$dirt"
  rc=0; out="$(env -u TINY_TOKEN WIFI_SSID=ap WIFI_KEY=k HOME="$T" TINY_HOME=/nonexistent TINY_API=http://127.0.0.1:9 TINY_IDF=host \
       PATH=/usr/bin:/bin STICKY_ESPTOOL_PYTHON="$T/fakepy" bash "$T/repo/tools/provision_new.sh" --board fnk0104s "$T/port" 2>&1)" || rc=$?
  { [ "$rc" = 2 ] && grep -q "uncommitted firmware/ or vendor/ source" <<<"$out" && ! grep -q "building\|enrolled" <<<"$out" \
    && ! grep -q write_flash "$T/py/esptool/__main__.py.calls"; } \
    || { echo "$out"; echo "FAIL: dirty $dirt did not refuse before build (rc=$rc)"; exit 1; }
  echo "  real run, dirty $dirt: REFUSED before build/enrol/flash"
done
# not over-broad: an uncommitted doc is not compiled in, so the run proceeds to
# the build (which fails here at once: TINY_IDF=host, no idf.py on PATH)
git -C "$T/repo" checkout -q -- .
mkdir -p "$T/repo/docs"; printf 'note\n' >"$T/repo/docs/x.md"
rc=0; out="$(env -u TINY_TOKEN WIFI_SSID=ap WIFI_KEY=k HOME="$T" TINY_HOME=/nonexistent TINY_API=http://127.0.0.1:9 TINY_IDF=host \
     PATH=/usr/bin:/bin STICKY_ESPTOOL_PYTHON="$T/fakepy" bash "$T/repo/tools/provision_new.sh" --board fnk0104s "$T/port" 2>&1)" || rc=$?
{ grep -q "building fnk0104s" <<<"$out" && ! grep -q "uncommitted" <<<"$out"; } \
  || { echo "$out"; echo "FAIL: an uncommitted doc blocked the run"; exit 1; }
echo "  real run, dirty docs/x.md only: proceeds to the build"
echo "provision host test: ALL GREEN"
