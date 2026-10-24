#!/bin/bash
# heapmark host test — compiles the SHIPPED firmware/main/tiny_heapmark.cpp
# with TINY_HEAPMARK_HOST (the table only; no IDF heap or mutex), and checks
# tools/check_sdkconfig.py against fixture sdkconfigs: a colour build that
# carries every internal-RAM setting passes, one missing a setting fails, and
# a reTerminal build carrying a colour-only setting fails.
set -euo pipefail
cd "$(dirname "$0")"
c++ -std=c++17 -Wall -Werror -DTINY_HEAPMARK_HOST -I ../../firmware/main \
    ../../firmware/main/tiny_heapmark.cpp heapmark_cases.cpp -o /tmp/heapmark_test
/tmp/heapmark_test

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
CHK=../../tools/check_sdkconfig.py
cat >"$T/colour" <<'EOF'
CONFIG_TINY_BOARD_FNK0104S=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=512
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
# CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP is not set
# CONFIG_ESP_WIFI_IRAM_OPT is not set
# CONFIG_ESP_WIFI_RX_IRAM_OPT is not set
CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=10
CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y
CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH=y
CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC=y
CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y
EOF
python3 "$CHK" fnk0104s "$T/colour"
grep -v RINGBUF "$T/colour" >"$T/colour_missing"
if python3 "$CHK" fnk0104s "$T/colour_missing"; then
  echo "FAIL check_sdkconfig passed a colour build without RINGBUF_PLACE_FUNCTIONS_INTO_FLASH"; exit 1
fi
cat >"$T/ret" <<'EOF'
CONFIG_TINY_BOARD_RETERMINAL=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_ESP_WIFI_IRAM_OPT=y
# CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH is not set
EOF
python3 "$CHK" reterminal "$T/ret"
sed 's/^# CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH is not set/CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y/' "$T/ret" >"$T/ret_leak"
if python3 "$CHK" reterminal "$T/ret_leak"; then
  echo "FAIL check_sdkconfig passed a reTerminal build with a colour-only setting"; exit 1
fi
echo "heapmark host test: ALL GREEN"
