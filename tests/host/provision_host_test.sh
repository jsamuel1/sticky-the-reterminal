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

# dry run, no board: plan + routes, nothing written
out="$(env -u WIFI_SSID -u TINY_TOKEN TINY_HOME=/nonexistent TINY_API=http://127.0.0.1:9 \
       bash "$ROOT/tools/provision_new.sh" --board fnk0104s --dry-run --no-wifi --mac b8:1f:3f:c5:05:2c 2>&1)" \
  || { echo "$out"; echo "FAIL: dry run exited non-zero"; exit 1; }
for want in "name tiny-eba6" "board fnk0104s" "0x009000" "pairing code" "dry run"; do
  grep -q -- "$want" <<<"$out" || { echo "$out"; echo "FAIL: dry run lacks '$want'"; exit 1; }
done
echo "  dry run: tiny-eba6, NVS 0x9000, pairing route"
echo "provision host test: ALL GREEN"
