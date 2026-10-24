# Changelog

Firmware versions come from `firmware/main/tiny/tiny_version.h`; the verb
grammar version (`TINY_GRAMMAR_VERSION`) is what clients gate on. One line per
release, newest first.

## Unreleased

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
- **firmware** — colour boards remap BOOT: click = home, hold = push-to-talk
  (records until release; 15 s cap). The reTerminal keeps click = ask and
  hold = home.
- **firmware** — a voice ask that fails before it is sent (record or media
  upload) now replaces the home stream with a card naming the reason. Before
  this fix the glass stayed on "(( thinking ))". The upload error carries the
  free internal heap.

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
