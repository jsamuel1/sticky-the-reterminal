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
| `agent_host_test.sh` | `firmware/main/tiny_agent.cpp` | the roster preview a universe push paints from (`tiny_agent_roster_preview_add`) gives the same verdict and roster as the real add — normalisation, refusal, dedupe, full-roster eviction keeping `tiny` at row 0 — and leaves the live roster and NVS untouched; every mutation (`set`, `add`, `remove`, batch `roster_commit`) under injected NVS open / set_blob / commit failure leaves RAM and flash unchanged; the batch commit is one write and refuses a stale base; across process restarts (the restart is the reboot): saved state survives and a refused change does not come back, a pre-blob per-key store migrates, and an unreadable blob falls back; and 19 outcome-unknown write scenarios (a failed `set_blob` that landed or not, `ESP_ERR_NVS_REMOVE_FAILED` on the write or the restore, failed commit, failed restore write/erase/commit, failed read-back, first save with no blob) each answer `ESP_OK` / an error / `TINY_AGENT_UNSETTLED` matching what the next boot loads, and an unsettled store is rewritten by the next no-op mutation (212 checks) |
| `bootmark_host_test.sh` | `firmware/main/tiny_bootmark.cpp` | the boot flight recorder: `prev` reads -1 before any write, latches the crash stage, reports 9 after a clean run (6 checks) |
| `batt_curve_host_test.sh` | `firmware/main/tiny_batt_curve.cpp` | FNK0104S divider battery: median of a burst (a lone spike moves neither it nor the IQR gate), ×2 applied after the median, LiPo resting-voltage curve (monotone, clamped), no percent outside 3.0–4.35 V or with a >150 mV spread; no signed overflow for any int input, under UBSan trap mode (39 checks) |
| `fnk_battery_contract_host_test.sh` | `tiny_version.h`, `Kconfig.projbuild`, `AGENTS.md`, `tiny_sensors.h`, `tiny_display.cpp`, `tiny_main.cpp` | FNK help/docs and live source contracts advertise the driven GPIO 9 ADC estimate, board-specific boot transport, and 1 s shared burst; AGENTS grammar markers equal the shipped grammar macro |
| `dr52_host_test.sh` | `utf8_complete_len` + `copy_label_utf8`, extracted from `tiny_display.cpp` at build time | UTF-8-safe truncation of card labels: never cut through a multi-byte rune, Turkish and emoji cases, NULL/empty (10 checks) |
| `lcd_palette_host_test.sh` | `firmware/main/lcd_palette.h` + `tiny/tiny_png.h` with the shipped `canvas.cpp` | colour-panel path: accent plane + theme palette → big-endian RGB565 strips (every pixel of a real 320x480 Canvas, light and dark), rotation, dirty band incl. accent-only changes, the e-ink invariant (no plane = unchanged gray bytes), PNG gray/indexed decoded by python zlib (85 checks) |
| `lcd_canvas_host_test.sh` | shipped `firmware/main/lcd_canvas.cpp` + `canvas.cpp` against an asynchronous fake strip panel | a full-frame transfer failing after two delivered DMA strips marks panel state uncertain; restoring Canvas then requesting a partial forces all rows and repairs exact physical panel bytes; normal equal-frame partials return to no-op (9 checks) |
| `render_host_test.sh` | the shipped `tiny_display.cpp` + `canvas.cpp` + `font.cpp`, with the device's cJSON 1.7.18 and qrcodegen vendored in `host/third_party/` and IDF/FreeRTOS stand-ins in `host/render_shim/` | 18 cards (every type, real shell/onboarding shapes, stress text) at 800x480, 800x480 rotated 90°, 320x480, 368x448 and 368x448 rotated 90°: no ink in the outer 4 px, every tap region inside the panel, and every frame's FNV-1a matches `render_golden.txt` (90 frames). `RENDER_OUT=dir` writes the PNGs; `--update` re-pins after an intended layout change |
| `provision_host_test.sh` | `make_ssid`, extracted from `tiny_provision.cpp`, with an IDF-v5.4 `esp_read_mac` shim; plus `tools/provision_lib.py` and `tools/provision_new.sh --dry-run` | the enrol name is the softAP MAC's (base + 1): firmware == script on four vectors incl. the FNK's b8:1f:3f:c5:05:2c → tiny-eba6; flash plan from `flasher_args.json` per board, board/flash-size/NVS-overlap refusals, the `tiny/cfg` NVS blob, session (new / re-key / ONLINE refusal / adopt) and pairing (approved / denied / expired / claimed) against a mock tiny API; the claimed capability roster == the compiled `s_caps[]`; flash size per board read from the flashed source's sdkconfig defaults; fw_commit provenance (-dirty / nogit / stale sha refuse; an exact annotated-tag stamp resolves only through the source checkout's tag ref); a real run on uncommitted partition table or vendor driver refuses before build/enrol/flash, an uncommitted doc does not |
| `sd_policy_host_test.sh` | `firmware/main/tiny/tiny_sd_policy.h` (header-only) with the host `esp_err` shim | the SDMMC microSD probe rules over IDF v5.4 command traces: missing media only when nothing answered and the probe timed out; software/config failures stay errors; 1-bit retry only for response/data signal failures after the card answered (never empty/FAT-only/heap/config failures), and a narrow timeout cannot erase wide-bus card evidence; the `sd format yes` hint only when every transfer worked; widths and wrap-safe 2/4/8 s backoff (58 checks) |
| `sd_report_host_test.sh` | source guard over `tiny_node.cpp`, `sticky_sdcard_sdmmc.cpp`, `sticky_sdcard.cpp`, `hal_absent.cpp` | `status` and `sd status` take the sd object from one `sticky_sdcard_report()` (no direct present/mounted/info reads), the SDMMC report reconciles and reads under its lock, SDMMC `present` == mounted, every driver defines the report |
| `gen_commit_host_test.sh` | the shipped `firmware/main/cmake/gen_commit.cmake`, run with `cmake -P` on a scratch repo | `-dirty` covers every compiled input (firmware/main, project CMakeLists, partition table, board sdkconfig defaults, vendored component) and nothing else (docs, tools, gitignored build dir), and `TINY_FW_COMMIT_DATE` is the committer date in UTC; at an exact annotated tag the stamp stays tag-only (`v9.9.9`, `v9.9.9-dirty`) and `provision_lib.py provenance --source` proves it against the scratch repo's tag ref (passes at the tag, refuses dirty, without `--source`, and once HEAD moves past the tag) (19 checks; needs cmake) |

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
