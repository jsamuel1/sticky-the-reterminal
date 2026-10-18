# devbench image sets

Each subdirectory is one flashable set: a `manifest.json` plus its `.bin`
parts. The bench page lists every set, and `firmware/build` (via ESP-IDF's
`flasher_args.json`) appears automatically as `build-firmware`.

```json
{
  "chip": "ESP32-S3",
  "boards": ["fnk0104s"],
  "version": "free text shown in the picker",
  "note": "free text shown above the parts list",
  "flash_settings": {"flash_mode": "dio", "flash_freq": "80m", "flash_size": "16MB"},
  "parts": [{"offset": "0x0", "file": "bootloader.bin"}]
}
```

- `chip` is checked against the connected board before any write.
- `boards` is checked against the bench label; a mismatch needs a second confirm.
- `flash_settings` rewrites the bootloader header (esptool behaviour). Omit it to keep the image's own header.
- The binaries are gitignored: only this README is tracked.
