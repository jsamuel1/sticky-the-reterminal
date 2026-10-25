# AGENTS.md — for the agent reading this repository

You are looking at the firmware, docs and tools for **Sticky**: a Seeed
reTerminal Sticky (ESP32-S3, 3.97" 800×480 4-gray e-ink, touch, PDM mic,
buzzer, IMU, 750 mAh) running custom ESP-IDF firmware that makes it a body
for one tiny.technology agent. The device is a thin client. The intelligence
is server-side. Everything the device does is a verb it received or a page it
draws locally.

This file is the map. It is written so you can act correctly in your first
ten minutes without reading 33k lines. Where it conflicts with the tree, the
tree wins — then fix this file.

## 1. Orientation in 60 seconds

```
firmware/main/           ESP-IDF v5.4 app. tiny_*.cpp modules, headers in tiny/.
                         board/canvas/font/sticky_* = HAL adapted from Seeed's demo.
vendor/Sticky_dashboard_demo/components/   Seeed's drivers, byte-identical. NEVER edit.
docs/                    mkdocs-material site (first-person voice pages + third-person ledgers)
docs/dashboard/          a SEPARATE product: FastAPI + React owner dashboard (sticky.cagatay.my).
                         Excluded from the site build. ~18k LOC. Not needed to build firmware.
docs/install/            web installer: manifest.json + sha256sums.txt (parts are NOT committed)
tools/                   flash/monitor/provision/release/package scripts (bash + python)
tests/host/              plain C++ host tests against shipped firmware files (no board)
playground/tiny/         the Seeed Sticky Playground registry entry, verbatim layout
recovery/                factory-dump manifest + sha (the 32 MB dump itself is gitignored)
```

Versions live in exactly one place: `firmware/main/tiny/tiny_version.h` —
`TINY_FW_VERSION` (string, e.g. `0.27.0-u2`) and `TINY_GRAMMAR_VERSION`
(int, currently 26). CMake reads the first; the heartbeat and `status` report
both. Bump the grammar on **any** change to a verb, its args, or a reply
shape, and write a line in `CHANGELOG.md`.

Live truth about a device is never in a file. Ask it: `status` over the relay
(`sticky_display` tool → verb `status`, or `use_device invoke`). It answers
`fw`, `fw_commit`, `grammar_version`, battery, Wi-Fi, heap watermarks, SD,
`card_id` on glass, `locked`, `auth_halted`.

## 2. Build, flash, test — the exact commands

```sh
tools/idf.sh build                           # ESP-IDF v5.4 in espressif/idf:v5.4 (default; same image as CI)
                                             # → firmware/build/tiny_sticky.bin (+ bootloader, table, ota_data)
tools/build_board.sh fnk0104s                # board variant → firmware/build-fnk0104s (also: amoled18)
TINY_IDF=host tools/idf.sh build             # opt out: use a host IDF (source ~/esp/esp-idf-v5.4/export.sh)
source ~/esp/esp-idf-v5.4/export.sh          # flashing/monitoring only: they need the host's USB + esptool
STICKY_PORT=/dev/cu.usbmodemXXXX tools/flash.sh        # app only, 460800 then 115200 fallback
STICKY_PORT=/dev/cu.usbmodemXXXX tools/first_flash.sh  # blank board: all four parts, 45 s boot log
STICKY_PORT=/dev/cu.usbmodemXXXX tools/monitor.sh
WIFI_SSID=ap WIFI_KEY=k tools/provision_new.sh --board fnk0104s [--repo URL] [--branch REF] <port>
                                             # enrol + build + flash provisioned; --dry-run prints the plan
tests/run.sh                                 # host tests: askq, bootmark, sleep/wake — must stay green
```

- Builds go through `tools/idf.sh`, which runs `idf.py` in the pinned
  `espressif/idf:v5.4` image. The repo is mounted at its own host path, and in
  a worktree so is the shared `.git`, so `fw_commit` is the real sha. Do not
  call a bare host `idf.py build` in scripts. `release.sh`,
  `playground_package.sh` and `build_board.sh` all go through `idf.sh`. Inside
  the image (CI) it runs `idf.py` directly.

- Board variants: `choice TINY_BOARD` (Kconfig.projbuild) = reterminal
  (default) | fnk0104s | amoled18. Pins per board in `pin_config_<board>.h`,
  HAL set per board in `main/CMakeLists.txt`, run-time facts in
  `tiny/board_caps.h` (status `board`, `caps{}` and the derived `absent[]`).
  Card layout follows the panel: a short side < 480 px (FNK 320×480, AMOLED
  368×448) selects the compact profile in `tiny_display.cpp`
  (`apply_layout_profile`); the reTerminal frame is pinned byte-for-byte by
  `tests/host/render_golden.txt` — `render_host_test.sh --update` only for an
  intended layout change, and say which frames moved. Boards without an IMU
  have no auto-rotate (`rotate auto` stays manual). Colour boards layer
  `sdkconfig.defaults.<board>` on the base, use `partitions_16mb.csv`, log on
  USB-Serial-JTAG, and accept `ota` only for images stamped with their own
  board id (see §10b). Shared
  `tiny_*` code takes no board `#if` beyond touch/audio/buttons/console.
  A colour board that drives a part hal_absent.cpp stubs defines `HAL_*` in
  its pin_config (AMOLED: `HAL_PMU_AXP2101`, `HAL_RTC_PCF85063`).

- AMOLED-1.8 power: the AXP2101 powers the SoC and the panel and there is no
  schematic, so `sticky_pmu_axp2101.cpp` never writes a DCDC/LDO register —
  it logs them at boot. The PWR key is an AXP2101 IRQ wired to TCA9554 P5,
  not a GPIO; `tiny_button` polls INTSTS2 (short = next page, long = sleep).

- Colour: every board draws into the same 2-bit gray `Canvas`. Colour panels
  (`lcd_canvas.cpp`) attach a 4-bit accent plane beside it; the renderer sets
  a pen (`PenScope` in `tiny_display.cpp`) and `lcd_palette.h` maps
  (accent, gray) to RGB565 per theme. The e-ink never gets a plane, so its
  bytes cannot change. Card chrome takes `accent`; body text stays neutral.

- `tools/port.sh` resolves the port and **refuses to guess** when several
  CH343 "USB Single Serial" adapters are present (SO-101 servo buses share the
  chip; flashing one is destructive). Its embedded map is the owner's bench —
  set `STICKY_PORT` on any other machine.
- `firmware/sdkconfig` is gitignored; `sdkconfig.defaults` **is the record**.
  Two P0s were Kconfig defaults (mbedTLS pinned to internal RAM; the power
  latch). If you change a default, say so in the commit.
- `firmware/build/` is the release input for `tools/release.sh` and
  `tools/playground_package.sh`. Both refuse a `-dirty` build.
- Serial card console: any `{json}` line on UART0 renders as a card;
  `tools/render_card.py '{"type":"text","title":"hi","body":"…"}'`. A line
  containing `"config"` is a bench provisioning blob → NVS → reboot.
- CI: `.github/workflows/firmware-build.yml` runs on every push to `main`
  and every PR: it builds all three boards in `espressif/idf:v5.4` via
  `tools/build_board.sh`, refuses a `-dirty`/`nogit` stamp, runs
  `tests/run.sh`, and uploads one devbench image set per board
  (`devbench-<board>`, made by `tools/devbench/mkset.py`).
  `tools/devbench/fetch_ci.sh [board…]` pulls the latest green `main` run
  into `tools/devbench/images/ci-<board>/` — flash those, not local builds.
  On push to `main`, job `ota-bench` then pushes each board's app image to
  the owner's bench boards over the relay (§10b).
  `release.yml` and `docs.yml` stay workflow_dispatch only; the docs site
  publishes via `mkdocs gh-deploy` to `gh-pages` (legacy Pages).

## 3. Boot sequence — the contract (`tiny_main.cpp`)

Order is load-bearing. Do not reorder without reading the comments in the file.

1. `board_init()` **first** — owns the power latch (GPIO45/46). Without it the
   board powers itself off when the button is released.
2. `tiny_display_init()`, buzzer, restore silent/soft/rotation-lock from NVS;
   boards with no IMU (`board_caps.imu` false, FNK0104S) also seed the saved
   manual rotation (`rotq`) via `tiny_display_preset_rotation` before the splash
   **before** the boot chime (or mute un-mutes for one beep).
3. Cold boot: splash (`tiny` + bar). Deep-sleep wake (EXT1 button / timer) is
   a **glance wake**: no splash, paint home first, init inputs after —
   `first_paint_ms` ≈ 1.1 s is the acceptance, exposed in `status`.
4. Battery source (BQ27220 gauge or FNK GPIO 9 ADC divider) + RTC → system
   clock (the paint's only data deps).
5. `tiny_shell_init()` renders **home before any network** — navigation must
   work offline.
6. Auto-rotate, microSD (after first paint, never in the glance budget).
7. `tiny_config_load()`: networks present → `tiny_wifi_connect()` → SNTP →
   `tiny_node_start()` if `device_id`+`token` present. **No networks → 
   `tiny_provision_start()`** (softAP portal). This is the first-boot path.
8. Serial card console loop forever.

`tiny_bootmark_stage(n)` writes the stage to NVS synchronously so a
crash-looping trial names the stage on the next boot (`boot_mark_prev`).

## 4. Modules (`firmware/main/tiny_*.cpp`) — one line each

| module | owns |
|---|---|
| `tiny_node` | heartbeat 30 s (idle 60 s) · relay poll 5 s / 60 s after 2 quiet min · `dispatch_inner()` — **every verb lives here** · reply PATCH · 3-strike 401 auth-halt (token kept, rescue portal) · ask queue drain · OTA trial commit |
| `tiny_display` | card-spec JSON → canvas → refresh policy (full vs partial) · title-bar glance cluster · hit-regions published atomically at **display time** · screenshot snapshot · gallery frames · stream notes |
| `tiny_shell` | stickyOS: ring home→status→sensors→settings, drill pages wifi/ble/cfgqr, nav stack (bottom = home), on-glass keyboard (wifi password / DM compose / typed ask), universe roster |
| `tiny_touch` | GT911 task (prio 6) → hit-region resolution → local action or `ui_tap` event upstream |
| `tiny_button` | board GPIO press/hold · raw-GPIO UP+DOWN 1 s unlock chord where both keys exist (reTerminal) |
| `tiny_lock` | §11 pocket lockout: manual on every board; IMU auto only with local UP+DOWN unlock · gates touch/buttons/**mic capture entry** · `status.locked` |
| `tiny_orient` | IMU classify → auto-rotate (1 Hz e-ink; 300 ms AMOLED; 3 agreeing samples) · per-board axis tables in `tiny/tiny_orient_map.h` (host-tested) · feeds the supported lock detector at ≤1 Hz (disabled on no-chord boards) |
| `tiny_rotation` | manual rotation for boards with no IMU (header-only, host-tested): capability gate on `board_caps.imu`, strict `rot:*` tap ids, settings picker card, apply-then-save through `tiny_display_set_rotation`; NVS `rotq`, unset/invalid boots 0° |
| `tiny_audio` | PDM 16 kHz WAV into PSRAM (cap 15 s) · re-claims GPIO19/20 from USB-JTAG before every capture (deep-sleep wake mutes the mic otherwise) · buzzer LEDC GPIO48, silent/soft tiers |
| `tiny_upload` / `tiny_screenshot` | `POST /api/media` (PSRAM buffer) · framebuffer → 2-bit PNG → URL |
| `tiny_sensors` | live on-demand reads: SHT40, LSM6DS3TR-C, PCF8563, plus battery from BQ27220 or FNK GPIO 9 ADC (AMOLED: QMI8658, PCF85063A, AXP2101) · ADC burst cached 1 s so status/glass agree · nulls never fabricated |
| `tiny_time` | RTC→system at boot (`tiny_main` attaches the RTC first), SNTP→system→RTC on Wi-Fi; `clock_source` names itself |
| `tiny_wifi` | STA join + roaming `networks[]` walk · 30 s re-roam task · per-walk failure log (boot "confession" card on wrong password) |
| `tiny_config` | NVS namespace `tiny`, key `cfg`: ONE JSON blob {device_id, token, api, name, networks[]} · `merge_json` is the single owner of merge semantics (portal, serial, `config` verb all funnel here) |
| `tiny_provision` | softAP `tiny-XXXX` / `tinysetup` / 192.168.4.1 · `GET /` form · `GET /info` · `POST /setup` (JSON from the iOS app, or urlencoded from a human) → merge → reboot · also the **rescue portal** (APSTA) after auth-halt |
| `tiny_ota` | `ota [channel]`: `PUT /api/firmware/manifest` → sha256-pinned download to the other slot → reboot as rollback-armed trial · numeric `ver_cmp` refuses downgrade unless `force` · `ota <url> <sha256>` (v15): media-store URL only, no ver_cmp (pusher proves ancestry) · both refuse an image whose board stamp (offset 288, `tiny/tiny_ota_util.h`) is not this board's, before the slot is touched |
| `tiny_stream` | `play <manifest>`: all frames prefetched to PSRAM in the **node task** before the ack, then a socket-less metronome task |
| `tiny_askq` | NVS ring of 8 queued text asks; one drained per healthy heartbeat |
| `tiny_agent` | which public tiny answers (`agent` verb, roster in NVS, `@slug` badge) |
| `tiny_ble` | NimBLE observer, scan-only |
| `tiny_bootmark` | boot stage crumbs in NVS `tinyboot` |

Other NVS namespaces: `tinywifi` (breadcrumbs), `tinyaskq`, `tinyboot`.

## 5. The verb grammar (v26) and the parity rule

Dispatched in `tiny_node.cpp::dispatch_inner()` — **23 verbs** on every
board, plus `brightness` where `caps.brightness` is true (colour boards):

`render_ui` `status` `sensors` `say` `ask` `agent` `play` `voice` `screenshot`
`miccheck` `page` `rotate` `glance` `tap` `swipe` `scroll` `lock` `unlock`
`sleep` `ota` `messages` `config` `sd`

Advertised in the heartbeat `caps[]` (`tiny_node.cpp` ~line 233) — the same
**23** since 0.28.0 (grammar v12 added `sd`, which had dispatched unadvertised
since 0.26.0). Board-only verbs are not in `s_caps[]`: `tiny_node_caps()`
appends them from the board table, so the shared roster the provisioning
tools parse stays the same on every board. The rule: *advertise exactly what
dispatches*. When you add a verb, add it to BOTH, bump the grammar, write the
CHANGELOG line.

Reply contract: `PATCH /api/devices/relay {inReplyTo, payload:"<string ≤8000 B>"}`
where payload is a serialized JSON string `{"result":…[, "images":[{url,format}]]}`.
Unknown verb → help text, never silence. A *typed push* — payload `type` a
string other than `invoke`, e.g. the platform's late-reply `notify` — owes no
reply: it is dropped with no PATCH (grammar v26, `tiny_envelope_classify`),
because answering a burst of them starved real verbs for minutes. `config` accepts **only**
`{"networks":[…]}`; identity fields are refused by design.

Card types the renderer knows (`tiny_display.cpp render_*_card`): `text`
`list` `kv` `menu` `qr` `chart` `chat` `composite` `image` `gallery`
`keyboard` `agent_home`, plus `universe` (shell). Agent-facing docs list the
first eight; `docs/cards.md` is the grammar with a real-glass capture per type.

## 6. First boot (`tiny_onboard.cpp`, since 0.28.0)

The step is **derived**, never stored: no networks → `welcome`/`wifi`;
networks but STA down → `link`; STA up but no `device_id`+`token` → `pair`;
provisioned but NVS `tinywifi/ob_done` unset → `ready`; else `done`.
`tiny_shell_home()` re-enters the flow while it is active (long-press AI and
the bottom-edge swipe cannot escape into a dead agent-home). UP/DOWN inside
the flow = `ob:next`/`ob:back`. `page onboard <step>` previews any card on a
provisioned device — that is how the README screenshots were taken.

Rails it reuses: the shell's Wi-Fi scan page + keyboard (`[Type it here]` →
`TINY_PAGE_WIFI`, its `[Back]` pops to onboarding), `tiny_config_merge_json`
as the only writer, the softAP portal (now **APSTA** so on-glass scan works
while a phone uses the AP; its page is two independent forms, Wi-Fi and
pair). `tiny_main.cpp` feeds the Wi-Fi walk verdict to `tiny_onboard_note_link`
instead of painting the old confession card when the device is unprovisioned.

Pairing (Phase B, 0.31.0): on the pair step the board asks
`POST /api/devices/pair/start` (tiny-tech enroll's headless contract; the old
note here said `/pair/begin`, which does not exist) for a code, shows it 3+3
with a QR of `tiny.technology/devices/pair?code=`, logs one INFO line, and
reports it as `pair_code` in `status` and portal `GET /info`. It polls
`/pair/poll` every 3 s and reboots provisioned on approval. Module:
`tiny_pair.cpp` (own task); pure half `tiny/tiny_pair_util.h` (host test
`tests/host/pair_host_test.sh`). The secret (32 B, base64url) stays in RAM and
is never logged; `secretHash` is sha256 of the base64url STRING. The Phase A
paste path (`192.168.4.1/pair`) remains as [Use the portal].

The flow was tested end to end on 0.28.0 from an erased NVS (welcome → portal
→ Wi-Fi → link → pair → ready → home). One thing it found: `POST /setup`
overflowed the 4 KB httpd task stack while merging config — the portal's
httpd now runs on 8 KB. Not exercised: a phone camera on the QR (decoded from
the framebuffer PNG instead), on-glass keyboard join during onboarding.

Rules that bit here: a `qr` part in a scrolling composite must be sized
against the visible floor (`render_qr_card` fix in 0.28.0); every body line
costs the QR ~38 px — keep onboarding bodies to two lines; a tour that scrolls
is a manual (four items fit); never paint a live mic button from an untrusted
string (SSID injection, 0.15.1); hit-regions publish at display time; verify
each new card with `screenshot` — three of five onboarding pages needed a
fix that only the glass showed.

## 7. Release discipline (`docs/release.md`)

1. Publish builds come from a **clean detached worktree**
   (`git status --porcelain -- firmware/ vendor/Sticky_dashboard_demo/components/` empty).
2. Labels are monotonic; a burned label is never reused.
3. Receipts outrank version strings — verify by making the surface answer.
4. **Never publish a `-dirty` or unpushed `fw_commit`.** (`gen_commit.cmake`
   injects `git describe` at build time; `-dirty` covers everything compiled in:
   `firmware/` and the vendored components. `tests/host/gen_commit_host_test.sh` pins it.)
5. Prove ancestry before publishing: `git merge-base --is-ancestor <on-glass> <build>`.
6. Claim a feature only after string-verifying the artifact.

Release rail: `tools/release.sh [--build] [--deploy]` rewrites
`docs/install/manifest.json` + `sha256sums.txt` from `firmware/build/`; the
mkdocs hook `tools/install_assets_hook.py` copies the four parts into
`site/install/fw/` and refuses bytes that don't match the sums. Tags are
`v<TINY_FW_VERSION>`.

## 8. Sticky Playground registry (Seeed) — what we ship and how

Registry: `Seeed-Projects/reterminal-sticky-playground-registry`. Layout is
`firmwares/<id>/` with a **strict** `firmware.json` (unknown keys fail
validation), `README.md`, `assets/preview.*` (real Sticky screenshot or
photo), and for firmware-only entries `firmware/<version>/manifest.json` +
`.bin` parts. Community entries: `group/catalogSection = community`,
`mode = flash`, `category` required, `author.name`, `support.url`,
`source.url` + `source.license`, `assets.previewAlt`.

`playground/tiny/` in this repo **is** that directory, verbatim. Produce it
with `tools/playground_package.sh [--build] [--validate]`: reads offsets from
IDF's `flasher_args.json`, writes sizes + sha256, rewrites
`flash.versions[0]`, refuses `-dirty`, and with `--validate` copies into a
registry clone and runs `npm run validate`. PR = copy the directory to
`firmwares/tiny/`, fill the physical-device test table in the README, use
the registry PR template (device revision, install method, main workflow,
reboot state, USB reconnect + reinstall).

Gates before the PR: repo public, license question for `vendor/` answered
(Seeed components ship with no license text; ask Seeed, do not assume), a
tagged clean build, the test record filled from a real flash of the exact
package. `PLAYGROUND.md` at the root is the short version of this section.

## 9. Docs conventions

- Voice pages (`index.md`, `start/quickstart.md`, `commands.md`, `cards.md`,
  `cookbook.md`, `stickyos.md`, `hardware.md`, `troubleshooting.md`) are
  **first person** — Sticky speaks. Ledgers (`ARCHITECTURE.md`,
  `API_CONTRACT.md`, `release.md`, `RECOVERY.md`) are third
  person. Don't mix.
- Front matter `readtime: N` and `description:` on every page. mkdocs
  builds `--strict`; a dead anchor is a red build.
- Every glass image is a real framebuffer (`screenshot` verb). No mock-ups.
  `docs/img/glass/INDEX.md` records how each capture was produced.
- Counts and pins in prose (verb count, grammar version, fw line) drift.
  When you change one, grep for the old number across `docs/` and `README.md`.
- `docs/dashboard/` is excluded from the site.

## 10. Probing a live device without lying to yourself

- `status`/`sensors` do not repaint — the right first probe. `render_ui`,
  `say`, `page`, `tap`, `swipe`, `play` claim the glass.
- The owner may be holding the device. Look at `idle_for_s`, `card_id`,
  recent `ui_tap` events before painting anything. Restore home after.
- Pending envelopes execute at **drain time**, not send time. A damning
  receipt needs a timestamp and a fresh reply; three confirmations, same
  actor, seconds apart, before calling a P0.
- A reply is not a frame. `rendered card_id=…` proves the renderer parsed;
  only a `screenshot` proves pixels.
- Rotation is runtime state: screenshot → tap coordinates must be atomic.

### 10a. USB bench: `tools/devbench` (the default for boards on the owner's desk)

The boards plug into the **owner's laptop**; the agent often runs on a remote
Linux box with no USB. `tools/devbench` bridges that: a stdlib server here,
a Web Serial page in the owner's Chrome/Edge (Safari has no Web Serial).

```sh
setsid nohup python3 tools/devbench/server.py >> .logs/devbench.log 2>&1 < /dev/null &   # 127.0.0.1:8765
curl -s -X POST "$KERMES_DASHBOARD_ORIGIN/api/browsers/open" -H 'content-type: application/json' -d '{"url":"8765"}'
```

- The owner picks a board label, clicks **Connect…**, then **Probe**, **Reset + capture boot log**, **Dump** or **Flash…**.
  Probe, boot log and dump are read-only. Flash writes, and always asks first with a confirm naming the board, the image set and whether the chip is erased.
- **The agent drives through jobs.** `tools/devbench/job.py probe|boot|dump|flash --label <board> ...` queues a request.
  - The page shows it as a banner with Run / Decline. Nothing runs without the owner's click.
  - `job.py wait <id>` blocks until the result lands in `jobs/done/<id>.json`.
  - Queue one job and wait for it. Don't stack writes.
- **Dump**: `--what all | partition:<name> | 0x8000:0x1000`.
  - It streams to `tools/devbench/dumps/<label>-<mac>-<what>-<ts>.bin` in 1 MiB uploads.
  - It is verified twice: the board's own MD5 of the range against what was read, and the server re-hashing its copy.
  - The result goes in `dumps/<name>.json` with `verified: true|false`. Never use an unverified dump.
- **Flash**: image sets come from `GET /api/sets`.
  - `firmware/build` appears automatically as `build-firmware`, read from IDF's `flasher_args.json`.
  - Any `tools/devbench/images/<set>/manifest.json` is listed too; see `images/README.md`. Binaries are gitignored.
  - Every part is MD5-checked after download.
  - Before the first write the page checks the chip type, flash size, and that nothing is placed below 0x8000 except at the chip's bootloader offset.
  - After each part, esptool-js has the chip hash the written range. A mismatch fails the job.
  - `--erase-all` wipes NVS (Wi-Fi, pairing token). Only use it when asked.
- `python3 tools/devbench/selftest.py` checks every server endpoint against the live server, no board needed. Keep it green.
- Read the results from disk; don't ask the owner to paste them.
  - `tools/devbench/reports/latest-<label>.json` holds the chip, features, MAC, flash JEDEC ID/size, bootloader header, partitions, app descriptors, boot log and parsed boot facts.
  - `reports/devbench.log` gets every console line within ~1 s, stamped with session and label.
  - `reports/`, `dumps/` and `jobs/` are gitignored.
- Reports are auto-sent after every probe and every boot capture.
- Under the dashboard proxy, requests arrive with `Transfer-Encoding: chunked`.
  The server handles that; any new endpoint must use `_body()`.
- The page reads flash at **1152000** baud by default (owner's choice), with a dropdown fallback.
  On native USB-Serial/JTAG (`303a:1001`) the baud setting is nominal.
- The Web Serial host buffer is 64 KiB.
  Flash reads use `readFlashExact` in bench.js, not esptool-js `readFlash()`. 0.7.0 never consumes the stub's trailing 16-byte MD5 frame, which corrupts the NEXT read ("serial noise" / timeout on the second read). `readFlashExact` reads and checks that frame, in 64 KiB chunks.
- Run the server with `setsid nohup`. A Kermes `background_job` server was
  reaped mid-session (2026-10-18).

Measured 2026-10-18 (both on `303a:1001`, ESP32-S3 QFN56 v0.2, embedded PSRAM 8 MB AP octal):

| label | MAC | flash JEDEC | bootloader hdr | console |
|---|---|---|---|---|
| `fnk0104s` | b8:1f:3f:c5:05:2c | 0x18405e Zbit, 16 MB | dio / 16MB | silent on USB (factory fw logs to UART0 header) |
| `amoled18-v2` | 44:bd:8d:78:30:b4 | 0x184020 XMC, 16 MB | dio / 16MB | USB; app at 0x200000, flash io qio, CO5300 + CST816S-driver touch at 0x15 (IC id 183), AXP2101, PCF85063A |
| `cyd-2432s028` | 68:09:47:f8:c1:2c | 0x164068 Boya, 4 MB | @0x1000 (ESP32) | CH340 `1a86:7523`; ESP32-D0WD-V3 rev 3, **no PSRAM**; factory Arduino sketch ("Setup done"), 2×1280K OTA + 1472K spiffs |

- CYD (ESP32-2432S028) USB-C: computer USB-C ports give it no power, most likely because the CC pull-downs are missing.
  Use a USB-A→C cable.
- On CH340, esptool-js's baud change re-opens the port, which resets the board.
  It then falls back to 115200 on its own, so probes work but slowly.
- The first stub read after each command reports "serial noise"; the per-chunk retry succeeds.

### 10b. Bench OTA: CI pushes every green `main` build to the bench

Job `ota-bench` in `.github/workflows/firmware-build.yml` runs on push to
`main` after `build` and `host-tests`. For each board with an entry in
`TINY_BENCH_DEVICES` it downloads `devbench-<board>`, and
`tools/ota_push.py` does: `status` (board must match, OTA must not be
`absent`) → `git merge-base --is-ancestor <on-glass fw_commit> <sha>`
(release rule 5; a downgrade, a dirty or unknown on-glass commit is refused)
→ `POST /api/media` → relay `ota <url> <sha256>` → poll `status` until
`fw_commit` is the pushed sha (5 min; the old commit coming back = rollback =
failed job). Runs never interleave (`concurrency: ota-bench`).

Secrets (repo jsamuel1/sticky-the-reterminal). The device map is keyed by
**device id**, not name (names collide; tiny-eba6 once enrolled as tiny-ed39):

```sh
tiny-tech login                       # 90-day user session JWT → ~/.tiny/credentials.json
python3 -c 'import json,os;print(json.load(open(os.path.expanduser("~/.tiny/credentials.json")))["token"])' \
  | gh secret set TINY_TOKEN -R jsamuel1/sticky-the-reterminal
gh secret set TINY_BENCH_DEVICES -R jsamuel1/sticky-the-reterminal \
  -b '{"fnk0104s":"4e6ed293-f54f-48e7-88d5-330c84234cd8"}'
```

- A missing secret or a board with no entry is a `::notice::` and a skip,
  never a failure. A board that HAS an entry but is offline fails the job:
  the bench was not updated. A JWT with under 14 days left fails the job loudly; refresh
  with the two commands above. The token is never printed.
- Each `build` job runs `tools/ota_push.py --inspect` on its app and fails
  unless the stamp is `ok`, its own board, project `tiny_sticky`.
- Only a reply starting `OTA staged` counts as success; every refusal
  (`... - nothing staged`, `already up to date`) fails the push.
- Bootstrap: an image from before grammar v15 refuses OTA on the colour
  boards and has no board stamp, so the first v15 image goes on by USB
  (devbench). From then on CI keeps the bench current.
- Prove the guard by hand (the board must stay on its slot and the reply must
  say "board guard"):
  `TINY_DEVICE=<id> tools/ota_push.py --creds --board fnk0104s --sha HEAD --image firmware/build-amoled18/tiny_sticky.bin --board-override`.
- `tools/ota_push.py --inspect <bin>` prints an image's stamp.
- Cloudflare 403s ("error code: 1010") the default `Python-urllib`
  User-Agent on tiny.technology; the pusher sends its own.

## 11. Things that are personal, secret, or must never be committed

- `.secrets.*.json`, `.logs/`, `recovery/*.bin`, `firmware/sdkconfig`,
  `docs/dashboard/.anim_frames/`, `.claude/` — all gitignored. Check
  `git status` before pushing; use `git add -u` or explicit paths, never
  `git add -A`.
- The device token (`tind_…`) is the only credential on the device; it lives
  in NVS, not in the repo. Never echo it in a card, a log line, or a receipt.
- `sticky.cagatay.my` is the owner's dashboard host. It appears as an
  env-overridable default (`STICKY_PUBLIC_URL`, `Kconfig TINY_DASHBOARD_URL`,
  CORS). Not a secret, but when open-sourcing, keep every occurrence
  overridable and say so.
- `tools/port.sh` embeds the owner's three boards (port, device-id prefix,
  MAC). Harmless but personal; `STICKY_PORT` overrides.

## 12. Pitfalls a new agent hits in the first hour

- **Shared worktree.** Two editors on one checkout lose hunks between the
  editor and `git add`. Build in a worktree; commit small.
- **Task stacks.** `tiny_node` runs at 16 KB because big TLS transfers
  (screenshot upload, OTA) overflowed 8 KB; `node_stack_min_free` in `status`
  is the watermark. Any new HTTP in a fresh task must budget mbedTLS.
- **Internal RAM.** `internal_min_free` (MALLOC_CAP_INTERNAL) is the number
  that kills Wi-Fi/TLS; `heap_free` is PSRAM-dominated and lies. Gate: >60 KB
  under load. mbedTLS and NimBLE pools are already in PSRAM. `status`
  `internal_boot` (v24) gives free/largest after each boot stage, so a new
  internal consumer shows as a bigger drop between two marks; add a
  `tiny_heapmark("<stage>")` after any new init. Non-DMA, non-ISR statics
  go in PSRAM (`EXT_RAM_BSS_ATTR`). `tools/check_sdkconfig.py <board>`
  asserts the colour boards' internal-RAM settings in a generated sdkconfig.
- **E-ink partial refresh** compares against the last *pushed* frame; a
  rotation change forces a full mono wipe. `blit_1bit` and image cards pin
  canvas rotation to landscape.
- **UTF-8.** Regions store raw UTF-8; folding to the 5×7 font's private bank
  (0x80–0x8B Turkish glyphs) happens only at paint; wire egress transliterates
  (`wire_fold_bank`). Truncate on rune boundaries (`utf8_complete_len`).
- **Provisioning strings are hostile.** SSIDs are 32 arbitrary bytes; always
  build JSON with cJSON, never `snprintf` into a JSON template.
- **`grep -q` under `set -o pipefail`** makes the producer die of EPIPE and
  the pipeline report failure — a "refuse if found" check silently passes.
  Capture into a variable instead (see `tools/playground_package.sh`).

## 13. When you are done

Run `tests/run.sh`. If you touched `firmware/`, build it. If you touched a
verb or a reply shape, bump `TINY_GRAMMAR_VERSION` and write the CHANGELOG
line. If you changed a number that appears in prose, grep for it. If you
painted the glass, restore home. Then update this file if it lied to you.
