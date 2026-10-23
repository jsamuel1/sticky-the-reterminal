---
title: Install
description: Put the tiny firmware on a reTerminal Sticky from your browser, or with one command.
readtime: 2
---

# Install

A reTerminal Sticky, a USB **data** cable, five minutes. Nothing to build,
nothing to sign up for yet. Two steps: put me on the board, then give me a brain.

## 1 · Put me on the board

<div class="install-hero" markdown>

<div class="install-card" markdown>

### In your browser

Chrome or Edge on a laptop — a phone cannot do this. Pick your board, plug it
in, press the button and pick its port. On the reTerminal Sticky that is
**USB Single Serial** (its CH343 bridge); the FNK0104S and AMOLED-1.8 show up as
**USB JTAG/serial debug unit** (the ESP32-S3's own USB). No driver on macOS,
Linux or Windows 10+.

<div class="install-form">
  <label>Board <select id="install-board"></select></label>
  <label>Wi-Fi <input id="install-ssid" placeholder="network name (optional)" autocomplete="off" maxlength="32"></label>
  <label>Key <input id="install-key" type="password" placeholder="empty = open network" autocomplete="off" maxlength="64"></label>
</div>

<div class="install-button-row">
  <esp-web-install-button id="install-sticky" manifest="manifest.json">
    <button slot="activate" class="md-button md-button--primary install-cta">Install Sticky <small>v0.28.0</small></button>
    <span slot="unsupported" class="install-note">This browser has no Web Serial. Use Chrome or Edge on a laptop — or the one-command path beside this.</span>
    <span slot="not-allowed" class="install-note">Web Serial needs HTTPS — you are on the right page, so this should not happen; reload.</span>
  </esp-web-install-button>
</div>
<p id="install-note" class="install-note" role="status" aria-live="polite"></p>

With Wi-Fi filled in, the flash also writes a small settings image holding that
network, so I join it on first boot and go straight to my pairing code. The
network name and key stay in your browser and on the board; this page sends
them nowhere.

A minute or two. Keep the cable in until the dialog says done; I reboot on
my own.

</div>

<div class="install-card" markdown>

### Or one command

macOS or Linux. It finds `esptool` (or installs it for your user), checks
every byte against `sha256sums.txt`, and flashes only if there is **exactly
one** Sticky-class USB bridge (WCH `1a86:55d3`) on the bus — it never guesses
a port.

```sh
curl -fsSL https://cagataycali.github.io/sticky-the-reterminal/install.sh | sh
# other boards: … | STICKY_BOARD=fnk0104s sh   (or amoled18)
```

Read it before you run it: [`install.sh`](install.sh). Rehearse without
touching the board with `STICKY_DRY_RUN=1`; pin a port with
`STICKY_PORT=<port>`.

To flash **and** enrol in one go (name from the board's MAC, your session or a
pairing code, Wi-Fi + identity written in the same flash), use
`npx tiny-tech flash --body sticky-the-reterminal`, which runs
[`tools/provision_new.sh`](https://github.com/cagataycali/sticky-the-reterminal/blob/main/tools/provision_new.sh)
from a checkout.

</div>

</div>

!!! warning "The first install erases the whole flash"
    A factory Sticky carries Seeed's demo, and my [NVS layout](../API_CONTRACT.md)
    starts empty — so the first flash wipes everything and starts clean. That is
    the honest default, not an accident. If you may ever want the factory image
    back, [dump it first](../start/quickstart.md#0-back-up-the-factory-brain-once-45-min);
    it needs the same cable and about 45 minutes — 32 MB at the one baud rate
    that never drops a byte.

## 2 · Give me a brain

My intelligence is not on the board. I am a 70-gram body for **your** tiny at
[tiny.technology](https://tiny.technology); the account is what makes me speak.

1. **Wi-Fi.** If you typed it above, I already have it. Otherwise I walk you through it on
   my screen (or open a Wi-Fi network called **`tiny-XXXX`**, key **`tinysetup`** —
   join it and open **http://192.168.4.1**).
2. **Pairing.** Once online I show a six-character code and a QR. Scan it, or open
   **tiny.technology/devices/pair**, sign in and approve me. I fetch my own device
   token; nobody copies one by hand.

!!! note "Why the page doesn't pair me itself"
    `npx tiny-tech flash` enrols the board from your computer before it boots. This
    page can't: tiny.technology's pairing API does not answer cross-origin requests
    from GitHub Pages (no `Access-Control-Allow-Origin`), so the browser is not allowed
    to read its replies. Pairing on the board's own screen gets you to the same place.

From here on I update myself over the air; the cable is only for emergencies.
The full walk, including the rescue path, is the [Quickstart](../start/quickstart.md).

## What ships

Four parts per board, one image each. Identity and Wi-Fi live in NVS
(`0x9000`, 24 KiB), not in the image.

| board | manifest | flash |
|---|---|---|
| reTerminal Sticky | [`manifest.json`](manifest.json) · [`sha256sums.txt`](sha256sums.txt) | 32 MB, dio @ 80 MHz |
| Freenove FNK0104S | [`manifest-fnk0104s.json`](manifest-fnk0104s.json) · [`sha256sums-fnk0104s.txt`](sha256sums-fnk0104s.txt) | 16 MB, dio @ 80 MHz |
| Waveshare AMOLED-1.8 V2 | [`manifest-amoled18.json`](manifest-amoled18.json) · [`sha256sums-amoled18.txt`](sha256sums-amoled18.txt) | 16 MB |

Parts sit at `0x0` (bootloader), `0x8000` (partition table), `0xf000` (OTA data)
and `0x20000` (app). Built by
[`tools/release.sh`](https://github.com/cagataycali/sticky-the-reterminal/blob/main/tools/release.sh)
from `firmware/build` and `firmware/build-<board>`; each manifest carries its
board's flash settings.

<script type="module" src="../js/esp-web-tools/install-button.js"></script>
<script type="module" src="board-picker.js"></script>
