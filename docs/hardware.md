---
readtime: 3
description: "Sticky's body pin by pin — ESP32-S3, 800×480 e-ink, GT911 touch, sensors, power path — verified against the running firmware."
---

# Hardware — my body

106 × 65.5 × 7.3 mm. 70 grams. IP40. N52 magnets in all four corners, so I
hold onto fridges and filing cabinets on my own. Everything below is read
from sources or the running silicon, not a brochure.

<figure class="sk-diagram sk-diagram--body">
--8<-- "assets/body.svg"
<figcaption>Where things are. Cyan is a pin I drive; gray is what the case gives me for free.</figcaption>
</figure>

| part | what |
|---|---|
| MCU | ESP32-S3 (QFN56 r0.2), 8 MB PSRAM, 32 MB QSPI flash |
| Display | 3.97" 800×480 e-ink, 235 ppi, 4-level gray, SPI (SSD1677-class) |
| Touch | GT911 capacitive, I²C0 |
| Sensors | SHT40 (temp/RH), LSM6DS3TR-C IMU, PCF8563 RTC, BQ27220 fuel gauge |
| Battery | 750 mAh |
| Audio in | PDM microphone |
| Audio out | magnetic buzzer on PWM — chimes, not speech |
| USB | USB-C, CH343 serial — recovery only; I update myself over the air |
| SDK | **ESP-IDF v5.4** |

## Pin map

Verified against three sources — Seeed's hardware overview, the community
`reterminal-sticky-2048` source, and my own `pin_config.h`, which agrees
pin-for-pin and adds the two power-path pins the others omit. When sources
disagreed, the running silicon won.

### Display — SPI2 (SSD1677 controller, 800×480 landscape native)

| signal | GPIO |
|---|---|
| MOSI | 14 |
| SCK | 13 |
| MISO | 12 |
| CS | 15 |
| DC | 16 |
| RST | 17 |
| BUSY | 18 |
| EN | 47 |

!!! warning "microSD shares SPI2"
    microSD CS is **GPIO8** on the same bus (card power **GPIO10**) — never drive it during a display transfer. FAT32/exFAT with MBR; hot-insert supported.

### Touch — GT911 on I²C0

| signal | GPIO |
|---|---|
| SCL | 2 |
| SDA | 3 |
| INT | 21 |
| RST | 41 |
| EN | 42 |

### Sensor bus — I²C1 (SHT40 · LSM6DS3TR-C · PCF8563 · BQ27220 @0x55)

| signal | GPIO |
|---|---|
| SCL | 0 |
| SDA | 1 |
| IMU INT | 7 |

<figure class="glass" markdown>
  <div class="glass__bezel"><img src="../img/glass/sensors.png" alt="Every device on this bus, read live: SHT40 temperature and humidity, IMU orientation, RTC time, gauge state of charge." width="800" height="480" loading="lazy" decoding="async"></div>
  <figcaption markdown="span">Every device on this bus, read live: SHT40 temperature and humidity, IMU orientation, RTC time, gauge state of charge.</figcaption>
</figure>

### Buttons, power, audio

| function | GPIO | notes |
|---|---|---|
| AI / Power button | 4 | deep-sleep EXT1 wake source |
| Up | 5 | |
| Down | 6 | |
| Power HOLD | 45 | must be held HIGH or the device powers off |
| Power LOCK | 46 | latch pulse low→high→low |
| Battery charge enable | 39 | `EN_BAT_CHGn`, **active low** (firmware source only) |
| PDM mic CLK | 19 | ⚠ see deep-sleep note |
| PDM mic DATA | 20 | ⚠ see deep-sleep note |
| Mic power enable | 38 | TPS22916 load switch |
| Buzzer PWM | 48 | |
| USB-C serial | TX 43 / RX 44 | reset = CHIP_PU |

!!! bug "Deep-sleep mic deafness (known silicon gotcha)"
    After a deep-sleep wake the ESP32-S3 pin mux reclaims GPIO19/20 for USB-Serial-JTAG, muting the PDM mic. Fix in `tiny_boot`, before I²S init: clear `USB_SERIAL_JTAG.conf0.usb_pad_enable`, `gpio_reset_pin(19/20)`, cycle the mic rail (GPIO38 low 150 ms).

## Display refresh modes

Three vendor HAL paths — 4-gray full (slowest, prettiest), monochrome full
(~600 ms), partial (fastest, ghosts). Which card uses which, and the 1.5 s
coalescing rule, are in [Cards → refresh policy](cards.md#refresh-policy).

## FNK0104S status light

The Freenove FNK0104S has one more light beside its touchscreen: a single
addressable RGB LED, not a second panel. What the 4.0" schematic and
Freenove's own sketches say, and what the firmware does with it:

| | |
|---|---|
| part | LED2, XL-5050RGBC-WS2812B — one pixel, GRB order (`Sketch_02.1_LedPixel`, `LEDS_COUNT 1`, `TYPE_GRB`) |
| data | net `RGB_LED` → ESP32-S3 pin 48, **GPIO42** (MTMS; not a strapping pin). DOUT is not connected, so there is no chain |
| power | VDD on the **+5** rail: USB VBUS through a B5819W diode, or the cell through the SL2305 switch — it lights on battery too. 3.3 V data drives a 5 V-powered WS2812B with no level shifter, as Freenove's demos do |
| shares | nothing with touch (FT6336U on I²C SDA 16 / SCL 15, INT 17, RST 18) or the LCD |
| driver | `espressif/led_strip` 3.0.3, one RMT TX channel, 48 symbols, no DMA (`tiny_led.cpp`) |
| config | `CONFIG_TINY_STATUS_LED` (FNK0104S only), `CONFIG_TINY_STATUS_LED_BRIGHTNESS` 1–64 of 255, default 8 |

What it shows, strongest first:

| colour | while |
|---|---|
| off | pocket-locked, or entering deep sleep (always) |
| blue | the mic is capturing |
| amber | the upload / ask is in flight |
| red | 5 s after an ask failed |
| magenta | a `"priority":"alert"` card is on glass |
| green | the platform reports unread DMs (heartbeat `unread` > 0) |
| off | otherwise |

The last two are steady: magenta clears when the next card commits, green
when the heartbeat reports zero. Image, gallery and universe cards honour
`priority` like any other card; gallery page flips and universe row taps
repaint the same page and keep its magenta. "The same page" is the
device's own gallery or universe page, not a card id: a sender's card that
happens to be named `gallery` or `universe` lends nothing to a later flip
or row tap. The reTerminal and AMOLED-1.8 have no such light and carry none
of this code.

Not yet verified on the physical FNK0104S: the colours as seen, every
transition above, touch while the light is lit, and whether 3.3 V data
latches reliably into the 5 V-powered WS2812B (VIH 0.7·VDD ≈ 3.5 V; no level
shifter). The host tests prove what the firmware tells the driver, not what
the LED emits.

## What this body cannot do

- **No speaker.** GPIO48 drives a magnetic buzzer via LEDC PWM: chimes and
  melodies, not speech. Voice *input* is first-class: mic → WAV → backend.
- **No video.** Full refresh is 1–2 s of physics; partial reaches ~300–500 ms
  with ghosting. "Stream" means time-lapse cadence, never smooth motion.
- **No LAN server.** I'm a client and poll for my work; nothing reaches me
  behind NAT. The provisioning portal is the one exception, alive only until
  Wi-Fi is configured.
