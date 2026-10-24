# Changelog

Firmware versions come from `firmware/main/tiny/tiny_version.h`; the verb
grammar version (`TINY_GRAMMAR_VERSION`) is what clients gate on. One line per
release, newest first.

## Unreleased

- **status buffer cap** — both `tiny_node_status_json` callers (the
  `status` verb and the settings page) declare their PSRAM buffer with one
  `TINY_STATUS_JSON_CAP` (2048) from `tiny/tiny_node.h`. A 1024 B caller made
  `status` answer `status json truncated` (1026 B at v17, 1303 B measured at
  v18), which stopped the bench OTA pre-status. `tests/host/status_cap_host_test.sh`
  fails on any caller not sized by the constant, or a cap under 1536 B. Reply
  shape unchanged, so the grammar is unchanged.

- **firmware, grammar v20** — FNK0104S battery percent from the GPIO 9
  divider. `sticky_battery_adc.cpp` now implements the whole battery API:
  a 15-sample burst goes to `tiny_batt_curve` (median, ×2 after the median,
  LiPo resting-voltage curve). No percent when the reading is outside
  3.0–4.35 V or the burst's interquartile spread exceeds 150 mV, so `status`
  `battery_pct` is null there and `battery_mv` still reports the measurement.
  `charging` is null (a divider sees no current); auto-sleep treats "cannot
  tell", and any divider read that is not a plausible cell, as wall power.
  A valid 0% now shows in the title bar and onboarding (it was hidden as
  "no reading"), and a failed read paints n/a on the sleep card, not 0%. `caps.battery_gauge` is on and `absent` drops
  `battery`, so the title-bar battery glyph and status card read real data.
  `sensors` battery gains `source` (`bq27220`|`adc`); an `adc` object carries
  only percent, voltage_mv, charging:null and present. `glance` battery_pct
  is null, not 0, with no reading.
  The `sensors` summary now counts `errors[]` as `(N errors)` (it said
  `unread`, which misnamed the FNK no-plausible-cell fault), and the sensors
  card footer names the GPIO 9 divider when that read fails;
  `tests/host/sensors_summary_host_test.sh` pins both.- **tests** — `tests/host/batt_curve_host_test.sh` (39 checks, built with
  UBSan in trap mode): median, spike tolerance, divider precision, curve,
  plausibility gates, no signed overflow across INT_MIN..INT_MAX.

- **firmware, grammar v19** — direct-artifact OTA now classifies every
  two-argument form, plus any one-argument value with a scheme (`http://`,
  `ftp://`, …), as direct. Exactly two arguments reach the strict URL/sha
  guards; extra arguments get the usage refusal. Invalid direct URLs can no
  longer be read as channel names with argument 2 silently ignored. Bare
  one-argument channel names and bare `ota` keep their channel behavior.
  `tools/ota_push.py` counts only
  `OTA staged` as success (`... - nothing staged` used to pass), and a
  configured bench board that is offline now fails the job instead of
  skipping. Each CI `build` job asserts its app's board stamp
  (`--inspect`: ok, own board, `tiny_sticky`). `tests/host/ota_push_cases.py`
  covers the reply classes and the offline exit.

- **firmware, grammar v18** — card layouts fit the small colour panels.
  A panel whose short side is under 480 px (FNK0104S 320×480, AMOLED-1.8
  368×448) gets a compact profile: 16 px margin, wrapping by the true glyph
  advance, a kv value column after the widest key (a long atomic value takes
  its own indented line instead of shrinking to 7 px glyphs), menu notes
  stacked under the label, titles drop to body scale before they are cut,
  one label scale per button bar, `...` on cut footers, and a wrapped
  unknown-type line. The card JSON is unchanged and the reTerminal frame is
  byte-identical. `status` gains `caps{}` (the board table) and `battery_mv`;
  the FNK0104S reads pack voltage from the GPIO 9 divider (calibrated ADC)
  and keeps `battery_pct` null, because it has no gauge. On a board with no
  IMU, auto-rotate is refused (`rotate auto` stays manual) and settings
  hides the follow-gravity row. `caps` includes the colour fields
  (`color`, `dark_theme`). The status buffers are now 2 KB.
- **tests** — `tests/host/render_host_test.sh` compiles the shipped
  `tiny_display.cpp` with the device's own cJSON 1.7.18 and qrcodegen
  (`tests/host/third_party/`). It renders 18 cards at 800×480, 480×800,
  320×480 and 368×448, fails on edge-band ink or out-of-panel tap regions,
  and pins every frame in `render_golden.txt`. Set `RENDER_OUT=dir` to get
  the PNGs. A 1 px reTerminal margin mutation fails all 36 reTerminal frames.
- **firmware/tools** — `fw_commit`'s `-dirty` now covers everything compiled
  into the image: all of `firmware/` (project CMakeLists, every board's
  sdkconfig defaults, partition tables) and the vendored Seeed components, not
  only `firmware/main`. An edited partition table or vendor driver used to
  build as a clean stamp. `playground_package.sh` gates on the same scope.
  `tests/host/gen_commit_host_test.sh` runs the shipped `gen_commit.cmake`
  against a scratch repo (compiled-input edits dirty, docs/tools/build dirs not).
- **tools** — `provision_new.sh` reads the enrolled capability roster
  (`tiny_node.cpp` `s_caps[]`, which had gained `sd`) and the board's flash
  size (`sdkconfig.defaults[.<board>]`) from the source it flashes instead of
  hand-kept copies. It refuses uncommitted `firmware/`/`vendor/` source before
  building, and a build whose `fw_commit` is `-dirty`, `nogit` or not the
  source HEAD (stale `--no-build`), unless `--allow-dirty`; exact annotated-tag
  stamps are verified against that selected source checkout's tag ref. The browser
  installer's `provision.js` roster (a static page cannot read the source)
  gains `sd` too; `tools/devbench/selftest.py` now fails when it drifts from
  `s_caps[]`. `playground_package.sh` reads the `-dirty` stamp from the build's
  `tiny_commit.h` (as CI does); its bare-sha pattern missed tagged stamps such
  as `v0.28.0-dirty`.

- **firmware** — colour UI on colour boards, grammar v16. The Canvas gains an
  optional 4-bit accent plane beside the unchanged 2-bit gray plane; only
  `lcd_canvas` attaches one, so the reTerminal's canvas is byte-for-byte what
  it was. `lcd_gray4.h` becomes `lcd_palette.h`: 16 accents × 4 levels per
  theme (light; dark = light-on-black, used by the AMOLED), expanded to RGB565 per
  strip, and the partial refresh diffs both planes. Caps gain `color` and
  `dark_theme`; FNK0104S (light) and AMOLED-1.8 (dark) `color` on.
- **grammar v16** — cards, composite parts and button objects take an optional
  `accent` (`red|orange|yellow|green|teal|blue|purple|pink|neutral|mono`,
  default blue) that tints chrome only; ignored on the e-ink. An `alert` title
  is red on colour glass. `screenshot` on a colour board is an 8-bit indexed
  PNG whose PLTE is the glass's palette (the e-ink keeps the 2-bit gray PNG).
  `status` gains `color` and `theme`. `page palette` previews the accents.
  Raw `image`/`gallery`/stream frames stay gray (literal, never themed); no
  RGB565 raw format.
- **tests** — `tests/host/lcd_palette_host_test.sh` replaces `lcd_gray4`:
  palette, accent plane, rotation, strips in both themes, dirty band on an
  accent-only change, the e-ink invariant, and the shipped PNG encoder
  (`tiny/tiny_png.h`) decoded by python zlib.

- **firmware** — AMOLED-1.8 V2: the CO5300 368×448 AMOLED is driven
  (`lcd_panel_co5300.cpp` behind `lcd_canvas`; replaces the headless canvas,
  which is removed). QSPI on SPI2 (CS 12, PCLK 11, D0–D3 4–7) at
  `CONFIG_TINY_LCD_SPI_MHZ` (40), RGB565, `espressif/esp_lcd_co5300` 2.2.0.
  Init table and x-gap rule are the Waveshare BSP 2.0.3's (x-gap 16 when
  the CST816S-class touch answers at 0x15), except brightness starts at 0 so
  power-on panel RAM is never lit. Brightness is DCS 0x51; sleep is dark +
  DISPOFF + SLPIN, wake SLPOUT + DISPON; the TCA9554 rail stays up.
  `LcdPanelOps` gains `row_align` (CO5300: 2): partial refreshes widen the
  dirty band to even rows. Canvas, screenshot and status `display` are now
  368×448 (`"368x448 AMOLED (4-gray canvas)"`) on this board; no verb or
  reply shape changed, so the grammar is unchanged. reTerminal and FNK0104S
  behaviour unchanged.

- **firmware + CI** (grammar v15) — bench OTA. Every app image now carries a
  32-byte board stamp (`TINYBRD1` + board id) in `.rodata_custom_desc`, which
  IDF links straight after `esp_app_desc_t` (image offset 288). `tiny_ota`
  reads it from the first 320 downloaded bytes and refuses a foreign,
  unstamped or truncated image before `esp_ota_begin`, so the inactive slot
  is never touched. With that guard, fnk0104s and amoled18 accept OTA
  (`absent` drops `"ota"`). New verb form `ota <url> <sha256>`: url must be
  `https://plugin.tiny.technology/media/<key>`, no version ordering (the
  pusher proves git ancestry); sha256 pin and rollback-armed trial as before.
  `tools/ota_push.py` drives it (status → ancestry → upload → ota → status),
  and the `ota-bench` job in `firmware-build.yml` runs it after every green
  push to `main` for each board in `TINY_BENCH_DEVICES`. Host test:
  `tests/host/ota_host_test.sh` (parser cases + the Python twin on the same
  bytes + the real `.bin` of each local build).

- **tools** — `tools/provision_new.sh` is board-aware and builds from any
  source: `--board reterminal|fnk0104s|amoled18`, `--repo <url|path>`,
  `--branch <ref>` (temp clone unless it is this checkout's HEAD; the flashed
  sha is printed; a `-dirty` build refuses without `--allow-dirty`). Parts and
  flash mode/size/freq come from the build's own `flasher_args.json` (was a
  hard-coded 32 MB), NVS from the board's partition table. The enrol name is
  now FNV-1a over the **softAP** MAC (base + 1) — what the board broadcasts;
  the old base-MAC hash enrolled the FNK as tiny-ed39 while it said tiny-eba6.
  No session → a pairing code (`/api/devices/pair/*`) instead of a refusal;
  ONLINE same-name row refuses unless `--adopt`. `--dry-run` prints the plan.
  No host ESP-IDF needed: NVS is generated in `espressif/idf:v5.4`, esptool
  comes from IDF, python3 or a pinned venv; macOS and Linux. Logic lives in
  `tools/provision_lib.py`; `tests/host/provision_host_test.sh` covers it.
  `npx tiny-tech flash --body sticky-the-reterminal` still calls it with the
  port alone (board via `STICKY_BOARD`).

- **firmware** — FNK0104S + AMOLED-1.8: one ES8311 + I²S std module
  (`audio_es8311.cpp`). Pins and amp polarity come from the board pin map
  (FNK: MCLK 4/BCLK 5/WS 7/DIN 6/DOUT 8, amp GPIO1 active-LOW; AMOLED: MCLK
  16/BCLK 9/WS 45/DIN 10/DOUT 8, amp GPIO46 active-HIGH). Codec control via
  `espressif/esp_codec_dev` 1.6.2 on the shared I²C bus. The voice capture
  contract is unchanged: 16 kHz/16-bit mono WAV, 15 s cap. Chime names and
  RTTTL play as PCM tones. The `sticky_buzzer_*` beeps keep the reTerminal
  timings, and the amp is on only while a tone plays.
- **firmware** — FNK0104S caps `mic`, `buzzer` and `speaker` are now true,
  proven on hardware (chimes audible, voice ask round-trips). `status`
  `absent[]` drops them and the beacon sets its speaker bit. The answer
  arrives as text; spoken replies are not part of this change. The AMOLED-1.8
  stays false until it is tested.
- **firmware** — colour boards remap BOOT: click = home, hold = push-to-talk
  (records until release; 15 s cap). The reTerminal keeps click = ask and
  hold = home.
- **firmware** — a voice ask that fails before it is sent (record or media
  upload) now replaces the home stream with a card naming the reason. Before
  this fix the glass stayed on "(( thinking ))". The upload error carries the
  free internal heap.
- **firmware** — colour boards: an FNK0104S voice upload failed with
  `ESP_ERR_NO_MEM` (24 KB internal free against the 60 KB TLS pre-flight).
  Small mallocs now go to PSRAM first (`SPIRAM_MALLOC_ALWAYSINTERNAL=512`), the
  I²S DMA rings shrink to 4 × 240 frames, and the tone player's queue and
  stack live in PSRAM. The audio init line logs its internal-RAM cost.
- **firmware** — about 39 KB of large non-DMA statics move to PSRAM `.bss`:
  button-region tables, the `tiny_config_t` copies, the scan, portal and
  console buffers, and the upload sink. Internal `.data+.bss` on the FNK0104S
  image drops from 73.8 KB to 34.9 KB.
- **grammar v17** — `status` gains `fw_date`, the UTC commit date of
  `fw_commit`. The settings page `fw` row reads `<version> (<commit>)`, and a
  new `date` row shows that same commit date, so the glass says which build
  it runs.

## 0.31.0 — grammar v14 — on-device pairing code (onboarding Phase B)

- **firmware** — FNK0104S touch: FT6336U polled on the shared I2C bus
  (`board_sensor_i2c_bus()`), one finger, raw-point log on finger-down, same
  tap/swipe classifier and `ui_tap` events as the GT911 path. The sensor →
  physical-canvas axis map is one function (`tiny/tiny_touch_map.h`,
  host-tested against `Canvas::to_logical`). `tap`/`swipe` bounds follow the
  physical canvas on colour boards. Caps `touch` on. No verb or reply change.
- **firmware** — new `tiny_pair`: an unprovisioned board with Wi-Fi enrols
  itself through tiny.technology's headless pairing API (the contract
  `tiny-tech enroll` uses): `POST /api/devices/pair/start` with the sha256 of
  a 32-byte RAM-only base64url secret → a code; the owner approves it at
  `tiny.technology/devices/pair?code=…`; the board polls `/pair/poll` every
  3 s (backoff on errors), merges `device_id` + token + name and reboots.
  Expired/claimed codes renew by themselves; a denial waits for [New code].
  Own task, 15 s timeouts, IDF cert bundle; the secret is never logged.
- **onboarding** — the pair page shows the code (3+3) and a QR of the
  approval link; the Phase A paste portal stays as [Use the portal]. One INFO
  console line names the code and link (headless boards).
- **grammar v14** — `status` gains `pair_code`, `pair_url`, `pair_expires_s`,
  `pair`; portal `GET /info` gains `pair_code` + `pair_url`; `ob:pair`,
  `ob:portal`, `page onboard portal`.
- **tests** — `tests/host/pair_host_test.sh`: sha256, base64url, secretHash
  (Node golden vectors), code/url, merge body, poll classifier.
- **firmware** — FNK0104S drives its ST7796S 320×480 LCD. New board-neutral
  `lcd_canvas` (gray4 canvas → RGB565 DMA strips; partial = dirty rows only;
  sleep = backlight off + SLPIN) + `lcd_panel_st7796` (SPI2 IO_MUX,
  `CONFIG_TINY_LCD_SPI_MHZ` default 40, LEDC backlight GPIO45). The canvas is
  the panel: 320×480 portrait; `screenshot`, `tap`/`swipe` bounds and raw
  image/gallery/stream frames follow it (19200 | 38400 B on this board). No
  verb or reply shape changed; reTerminal output unchanged. AMOLED stays
  headless.

## 0.30.0 — grammar v13 — board variants (phase 1)

- **firmware** — `choice TINY_BOARD` in `main/Kconfig.projbuild`:
  reTerminal Sticky (default, unchanged HAL), Freenove FNK0104S, Waveshare
  ESP32-S3-Touch-AMOLED-1.8 V2. One pin map per board behind `pin_config.h`,
  HAL sources gated per board in `main/CMakeLists.txt`, and a run-time
  `board_caps` table read by boot, `status`, the beacon caps bits, auto-sleep
  and OTA. Colour boards build with `tools/build_board.sh <board>` into
  `firmware/build-<board>` (own sdkconfig; `firmware/build` stays the
  reTerminal release input) on `partitions_16mb.csv` (2 × 6 MB OTA).
- **firmware** — colour boards, phase 1: no latch, headless 800×480 canvas
  (cards render, `screenshot` works, nothing reaches a panel), absent
  battery/RTC/SHT40/IMU/buzzer/SD stubs answering `ESP_ERR_NOT_SUPPORTED`,
  BOOT (GPIO0) = OK/AI, console + `{json}` bench line on USB-Serial-JTAG.
  Boot logs flash id/size and PSRAM size/mode and probes the shared I2C bus;
  the AMOLED releases DSI_PWR_EN/LCD_RST/TOUCH_RST on its TCA9554 first and
  names the touch revision (CST820 0x15 = V2). No gauge = no auto-sleep.
- **grammar v13** — `status` gains `board` and `absent[]`; `ota` refuses on a
  board no channel carries.

## 0.29.0 — grammar v12 — TinyBeacon v2 presence

- **firmware** — the Sticky is findable over Bluetooth without pairing
  (tinyai-id `docs/ble-mesh/SPEC.md`). New `tiny_beacon`: NimBLE broadcaster +
  peripheral on the existing scan-only host — ADV_IND = flags + Catalog UUID +
  mfg SHORT (31 B exactly), SCAN_RSP = mfg FULL `'TN'` + 9-byte header (+ name
  only when social); Catalog GATT `identity` / `tools` (paged, ≤512 B) /
  `hello` / `endpoint` serving the 23 firmware verbs; session rotates every
  15 min (esp_random); 250–400 ms interval; connectable, advertising resumes
  after a disconnect. ON at boot, **anonymous** (kind + tool count, no name) —
  a device named after its owner must not leak it. `tiny_ble` now registers
  GATT before `nimble_port_freertos_init` (gap/gatt svc init were never called
  before). Caps 0x391c = fixtures `sticky_full`.
- **rollout** — first flashed to ONE bench Sticky over the `sticky-dev` channel
  (lane BLE-ROLLOUT, 2026-09-21); the other two follow after the catalog read
  from a Mac/phone is proven.

## 0.28.0 — grammar v12 — first-run onboarding

- **firmware** — a real first boot. New `tiny_onboard` module: welcome →
  wi-fi (phone QR to the portal, or the on-glass scan list + keyboard) →
  link (honest join verdict in-flow) → pair → a one-screen tour, then home.
  The step is derived from device state, never stored as a cursor. `home`
  (long-press AI, bottom-edge swipe) re-enters the flow until the device is
  paired and the tour dismissed (`ob_done` in NVS). UP/DOWN inside the flow
  = next/back. Touch namespace `ob:*`, card_ids `ob-*`.
- **firmware** — the provisioning portal comes up in **APSTA** so the
  on-glass Wi-Fi scan works while a phone talks to the AP; its page is two
  forms (Wi-Fi alone, or device id + token alone — each a valid step) with
  scanned networks as a datalist; `/pair` anchors the second form. The portal
  no longer paints its own card; the shell owns the glass. The pre-0.28 card
  promised "open the tiny app", which cannot onboard a Sticky (the app
  provisions Nicla over BLE only) — the copy now says what actually works.
- **firmware** — `render_qr_card` inside a scrolling composite: sized against
  the visible floor, top-aligned, real end-y. Before, `max_y` was the
  measuring horizon (1<<28), so any `qr` part in a composite painted
  off-canvas and left a phantom scrollbar. Whole `qr` cards were unaffected.
- **firmware** — the portal's httpd task runs on 8 KB, not the 4 KB default.
  The real first-boot test overflowed it on the pair form (`***ERROR*** A
  stack overflow in task httpd`): `tiny_config_merge_json` keeps two
  `tiny_config_t` on the caller's stack. Portal provisioning had only ever
  worked by luck. Also `box-sizing` on the portal page (form overflowed a
  390 px phone by 26 px).
- **grammar v12** — `page onboard [welcome|wifi|link|pair|ready]` previews
  any first-run card (screenshots for the registry README come from here);
  `status` gains `onboard:"<step>"`; heartbeat `capabilities[]` gains `sd`
  (dispatched since 0.26.0, never advertised — 23 = 23 now).
- **repo** — `AGENTS.md` (the map for agents), `playground/tiny/` (Seeed
  Sticky Playground registry entry, validated with Seeed's `npm run validate`),
  `tools/playground_package.sh` (refuses `-dirty` builds). `PLAYGROUND.md`
  rewritten for the registry's real `firmwares/<id>/` layout.

## Unreleased

- **repo** — the tree is firmware-only: the parametric case moved out so
  the site and the .bin are the only surface. Playground submission
  metadata lives in `PLAYGROUND.md`.

- **docs** — Quickstart leads with the web installer (browser button + `curl … | sh`);
  ESP-IDF moved to an optional "Build it yourself". The verb count is **23**
  everywhere (the `dispatch_inner()` table), with the `sd` drift stated plainly:
  23 dispatched, 22 advertised in the heartbeat `caps[]` array — a firmware fix,
  still open. `$STICKY_PORT` is defined once and carries the CH343 "USB Single
  Serial" ambiguity note; the owner's own port no longer appears in RECOVERY.
  `og:image`/`twitter:image` lose a doubled slash. Deep sleep is described
  honestly as ~0 mA = 10–20 µA (e-ink's zero-draw *hold* claim unchanged).
  The landing wears the same `sk-header`/`sk-footer` chrome as every inner page.

- **0.27.0-u2** (2026-08-29) — repair a duplicated tail in `tiny_touch.cpp`; builds green.
- **0.27.0-u1** — bounded `%u` on roster NVS keys (format-truncation fix).
- **0.27.0** (grammar 11) — the universe on the glass: `agent` verb family, universe card, `u:` touch namespace, `@slug` home badge, honest refusal rendering.
- **0.26.0** — microSD on the shared SPI2 bus: `/sdcard` mount, `sd` verb (status/ls/df/probe/format), gallery frames and photos archived to the card; 128 GB proven.
- **0.25.x** (grammar 9–10) — gallery card + `g:` namespace, composite scroll rail, power-latch hold through runtime, RTC black-box crumb, stack-overflow fix in `tiny_node`, device DM send fixed, ASCII fold of private font bytes at the wire.
- **0.24.0** — silent mode (buzzer funnel, NVS-persisted) and a batch of settings/status fixes.
- **0.22.0** (grammar 9) — `config` verb: WiFi roaming list in NVS, 30 s re-roam task, settings QR.
- **0.21.0** — ask lives on the home surface; keyboard v2 with symbols plane.
- **0.20.0** — agent-home card: the home screen is the agent UI.
- **0.19.0** (grammar 7) — image card: server-packed 1-bit / 4-gray canvas over HTTPS.
- **0.18.0** (grammar 6) — glance: title-bar cluster (unread, lock) redrawn by partial refresh.
- **0.17.0** — pocket lockout: lock module, UP+DOWN unlock chord, input gates.
- **0.16.2** (grammar 4) — `swipe` verb for gesture injection.
- **0.15.x** (grammar 3) — streamed asks render live on the glass; chat card.
- **0.14.x** (grammar 1–2) — the 14-verb grammar, body scroll, DM surface (`messages`), `fw_commit` in status.
