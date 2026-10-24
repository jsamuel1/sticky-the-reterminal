#!/usr/bin/env python3
"""check_sdkconfig.py — assert the GENERATED sdkconfig of a board build.

    tools/check_sdkconfig.py <board> [sdkconfig]

sdkconfig defaults to the board's build dir (firmware/sdkconfig for the
reTerminal, firmware/build-<board>/sdkconfig otherwise). Run it after a clean
reconfigure or build: a stale build dir keeps the values it was first
configured with, so only a generated file proves what the image was built
with (a local build-fnk0104s once still held ALWAYSINTERNAL=16384 after the
overlay said 512).

The rules are the colour boards' internal-RAM settings (tiny_upload needs
60 KB of internal heap for a TLS session) and the reTerminal's promise that
none of them leak into its build. Exit 0 = every rule holds.
"""
import os
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

# value None = the option must be absent or "is not set".
COLOUR = {
    "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL": "512",
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "32768",
    "CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP": None,
    "CONFIG_ESP_WIFI_IRAM_OPT": None,
    "CONFIG_ESP_WIFI_RX_IRAM_OPT": None,
    "CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM": "10",
    "CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH": "y",
    "CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH": "y",
    "CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC": "y",
    "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY": "y",
}
RULES = {
    "fnk0104s": dict(COLOUR, CONFIG_TINY_BOARD_FNK0104S="y"),
    "amoled18": dict(COLOUR, CONFIG_TINY_BOARD_AMOLED18="y"),
    # The reTerminal keeps the IDF defaults these overlays override.
    "reterminal": {
        "CONFIG_TINY_BOARD_RETERMINAL": "y",
        "CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL": "16384",
        "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "32768",
        "CONFIG_ESP_WIFI_IRAM_OPT": "y",
        "CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH": None,
        "CONFIG_RINGBUF_PLACE_FUNCTIONS_INTO_FLASH": None,
    },
}


def parse(path):
    vals = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line.startswith("# CONFIG_") and line.endswith(" is not set"):
                vals[line[2:-len(" is not set")]] = None
            elif line.startswith("CONFIG_") and "=" in line:
                k, v = line.split("=", 1)
                vals[k] = v.strip('"')
    return vals


def check(board, path):
    """Return a list of failure strings (empty = pass)."""
    if board not in RULES:
        return [f"unknown board {board!r} ({' | '.join(RULES)})"]
    vals = parse(path)
    fails = []
    for k, want in RULES[board].items():
        got = vals.get(k)
        if got != want:
            fails.append(f"{k}: want {'not set' if want is None else want}, "
                         f"got {'not set' if got is None else got}")
    return fails


def main(argv):
    if len(argv) not in (2, 3):
        print(__doc__, file=sys.stderr)
        return 2
    board = argv[1]
    if len(argv) == 3:
        path = argv[2]
    else:
        sub = "sdkconfig" if board == "reterminal" else f"build-{board}/sdkconfig"
        path = os.path.join(REPO, "firmware", sub)
    if not os.path.isfile(path):
        print(f"check_sdkconfig: {path} not found (build or reconfigure first)",
              file=sys.stderr)
        return 2
    fails = check(board, path)
    for f in fails:
        print(f"FAIL {board} {f}")
    if fails:
        return 1
    print(f"check_sdkconfig: {board} OK ({len(RULES[board])} rules, {path})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
