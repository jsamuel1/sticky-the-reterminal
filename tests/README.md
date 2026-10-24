# Tests

```sh
tests/run.sh          # all of them, ~5 s, exit code is the verdict
```

Everything here runs on the **host** with a plain C++17 compiler. No ESP-IDF,
no board. Each test compiles the *shipped* firmware source against the stubs in
`host/shim/` (`esp_err.h`, `esp_log.h`, `nvs.h` backed by a TSV file, a
FreeRTOS header that is all no-ops), so a test can never drift onto a stale
copy of the code.

| test | compiles | asserts |
|---|---|---|
| `askq_host_test.sh` | `firmware/main/tiny_askq.cpp` | the offline ask queue: FIFO order, depth-8 eviction, and that the queue survives a reboot — the script runs the binary twice against one NVS file; the second process *is* the reboot (20 checks) |
| `bootmark_host_test.sh` | `firmware/main/tiny_bootmark.cpp` | the boot flight recorder: `prev` reads -1 before any write, latches the crash stage, reports 9 after a clean run (6 checks) |
| `dr52_host_test.sh` | `utf8_complete_len` + `copy_label_utf8`, extracted from `tiny_display.cpp` at build time | UTF-8-safe truncation of card labels: never cut through a multi-byte rune, Turkish and emoji cases, NULL/empty (10 checks) |
| `lcd_palette_host_test.sh` | `firmware/main/lcd_palette.h` + `tiny/tiny_png.h` with the shipped `canvas.cpp` | colour-panel path: accent plane + theme palette → big-endian RGB565 strips (every pixel of a real 320x480 Canvas, light and dark), rotation, dirty band incl. accent-only changes, the e-ink invariant (no plane = unchanged gray bytes), PNG gray/indexed decoded by python zlib (85 checks) |
| `render_host_test.sh` | the shipped `tiny_display.cpp` + `canvas.cpp` + `font.cpp`, with the device's cJSON 1.7.18 and qrcodegen vendored in `host/third_party/` and IDF/FreeRTOS stand-ins in `host/render_shim/` | 18 cards (every type, real shell/onboarding shapes, stress text) at 800x480, 480x800, 320x480 and 368x448: no ink in the outer 4 px, every tap region inside the panel, and every frame's FNV-1a matches `render_golden.txt` (72 frames). `RENDER_OUT=dir` writes the PNGs; `--update` re-pins after an intended layout change |
| `provision_host_test.sh` | `make_ssid`, extracted from `tiny_provision.cpp`, with an IDF-v5.4 `esp_read_mac` shim; plus `tools/provision_lib.py` and `tools/provision_new.sh --dry-run` | the enrol name is the softAP MAC's (base + 1): firmware == script on four vectors incl. the FNK's b8:1f:3f:c5:05:2c → tiny-eba6; flash plan from `flasher_args.json` per board, board/flash-size/NVS-overlap refusals, the `tiny/cfg` NVS blob, session (new / re-key / ONLINE refusal / adopt) and pairing (approved / denied / expired / claimed) against a mock tiny API; the claimed capability roster == the compiled `s_caps[]`; flash size per board read from the flashed source's sdkconfig defaults; fw_commit provenance (-dirty / nogit / stale sha refuse); a real run on uncommitted partition table or vendor driver refuses before build/enrol/flash, an uncommitted doc does not |
| `gen_commit_host_test.sh` | the shipped `firmware/main/cmake/gen_commit.cmake`, run with `cmake -P` on a scratch repo | `-dirty` covers every compiled input (firmware/main, project CMakeLists, partition table, board sdkconfig defaults, vendored component) and nothing else (docs, tools, gitignored build dir), and `TINY_FW_COMMIT_DATE` is the committer date in UTC (11 checks; needs cmake) |

Each was checked by mutation: break the function under test and the test
fails (askq: 2, bootmark: 2, dr52: 4 checks go red; provision: hashing the
base MAC instead of the softAP MAC fails the firmware == script check;
disabling the uncommitted-source refusal fails the real-run case;
gen_commit: the old `firmware/main`-only scope fails 4 checks;
render: a 1 px reTerminal margin change fails all 36 reTerminal goldens).

What is **not** tested here: anything that needs the panel, WiFi or the
tiny.technology relay. That is verified on a real Sticky — `tools/monitor.sh`
and the `status` / `screenshot` verbs are the tools for it.

## Adding one

Copy the nearest `*_host_test.sh` + cases file, compile the shipped `.cpp`
(not a copy), add a shim header only if the module needs one, and make the
binary exit non-zero on any failed check. `run.sh` picks up every
`*_host_test.sh` automatically.
