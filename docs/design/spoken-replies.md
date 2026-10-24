---
readtime: 5
description: "Design for spoken agent replies on the codec boards: one verb, one ask opt-in, one reply field, 16 kHz PCM from the media store."
---

# Spoken replies — design (grammar v19, proposed)

Status: **proposal, no code**. Third person. It covers the agent's reply audio
(TTS) on the boards that have a real speaker: today the FNK0104S, and the
AMOLED-1.8 once its ES8311 is proven. Nothing here changes the reTerminal: its
"speaker" is a piezo buzzer and stays chimes-only.

## What exists

- `audio_es8311` runs I2S0 full duplex at **16 kHz, 16-bit stereo slots**,
  4 × 240-frame DMA rings in internal RAM, and a PSRAM-stacked `audio_play`
  task that renders tone lists (`TinyNote`) and switches the SC8002B amp
  around silence (30 ms lead, 120 ms tail).
- `tiny_pcm` is the host-testable PCM layer (WAV header writer, tone
  renderer, stats).
- `sticky_buzzer_silent()` / `sticky_buzzer_soft()` are the owner's mute and
  volume tier, persisted in NVS. Chimes honour both.
- `POST /api/devices/ask` answers SSE (`{"t":…}` deltas, `{"card":…}`,
  `{"error":…}`, `[DONE]`) or classic JSON `{text, card?}`. The `stream:"1"`
  body flag is an opt-in that an older backend ignores.
- `media_url_ok()` (`tiny/tiny_ota_util.h`) already pins
  `https://plugin.tiny.technology/media/<key>` for `ota <url> <sha256>`.
- Internal RAM is the scarce rail on the colour boards: a TLS session needs
  ~60 KB of it, and `tiny_upload` refuses below that.

Nothing on the device or in this repo synthesises speech, and the
tiny.technology backend is not in this repo.

## Decisions

### Format: WAV, PCM s16le, mono, 16 000 Hz

The same rate the I2S bus already runs, so there is no resampler and no
decoder. 32 KB/s; the 30 s cap is 960 044 bytes, which fits PSRAM (8 MB) many
times over and downloads in a few seconds over a TLS path that already moves
multi-megabyte OTA images.

Opus or MP3 were rejected for v19. They need a codec component (flash, a
decoder stack in internal RAM, CPU during playback) to save bandwidth the
Wi-Fi link does not need saved. Revisit only if a measured download time
makes speech start late.

The header is the format check, the way exact `Content-Length` is for
`/api/frames`: `RIFF`/`WAVE`, `fmt ` chunk with PCM (1), 1 channel,
16 000 Hz, 16 bits, then `data` whose size equals `Content-Length − 44`.
Anything else is refused with the field that failed. The parser goes into
`tiny_pcm` next to the writer so `tests/host` can pin it.

### Transport: a media URL, fetched after the text stream closes

The server synthesises the clip, stores it in the media store, and hands the
device a URL. The device fetches it **after** the ask's SSE connection has
closed. There is never a second TLS session open beside the ask, which the
internal-RAM budget cannot afford, and the text still appears at its current
speed: speech never delays the glass.

Inline audio in the SSE stream (base64 chunks) was rejected. It puts audio
bytes and the text deltas on one 30 s stall timer, inflates the payload by a
third, and makes the ask's line buffer carry audio.

The URL must pass `media_url_ok()`: the same host, path and key alphabet as
`ota <url>`, no other host. An optional `sha256` is verified when present.

### Playback: download whole, then play

v19 downloads the full clip into PSRAM, checks it, then plays it. Partial
playback while downloading (a PSRAM ring with a ~500 ms prebuffer) is a later
change, made only if measurements show the download, not the synthesis,
dominates the wait.

The player gets a second item kind beside tone lists: a PCM clip (pointer +
frame count into PSRAM). The task expands mono to the stereo slots in 240-frame
chunks, applies the volume tier as a gain, and keeps the existing amp
discipline (lead, tail, amp only around audio).

Rules:

| situation | behaviour |
|---|---|
| owner muted (`silent`) | not fetched, not played; the reply says `silent: not spoken` |
| volume tier soft | same clip, gain ÷ 8 (the chime ratio) |
| chime while speaking | dropped; the speech is the feedback |
| AI button, `voice`, `ask` or `miccheck` while speaking | speech stops first (barge-in), then the amp is off before capture starts, so the mic never records the speaker |
| new `speak` while speaking | the old clip stops, the new one plays |
| any new card or `render_ui` | speech continues; audio and glass are independent |
| pocket lock | speech allowed. Lock guards inputs, not output (same as `say`) |
| deep sleep requested | speech stops, the amp turns off, then sleep |
| clip longer than 30 s, bad header, wrong host, sha mismatch, `internal_free` < 60 KB | refused before playback, with the reason |

### Capability: `caps.speech`, separate from `caps.speaker`

`caps.speaker` already means "a sound comes out", and on the reTerminal that
is a buzzer (the beacon's historical bit). A new `speech` field means "PCM
playback through a codec and an amp". FNK0104S: true. AMOLED-1.8: false until
its ES8311 is proven on hardware. reTerminal: false.

The heartbeat `capabilities[]` lists `speak` only where `caps.speech` is
true, so the list stays exactly what dispatches. On the other boards the verb
answers `no speaker on this board (buzzer only)` instead of help text.

## Grammar v19 (proposed)

**Verb `speak`**

```
speak <media_url> [sha256]   fetch, check, play; reply after the check, before playback ends
speak stop                   stop now, amp off
speak status                 last verdict
```

Replies: `speaking 4.2 s (67 200 frames)`, or a refusal naming its reason
(`refused: not plugin.tiny.technology/media`, `refused: wav 22050 Hz, need
16000`, `refused: 38.0 s > 30 s cap`, `silent: not spoken`). `speak status`
reports the last run the way `play status` does:
`done 4.2 s, 0 tx stalls`, `stopped at 1.9 s (barge-in)`, `fetch failed: 404`.

**Ask opt-in.** `POST /api/devices/ask` gains `speak:"1"` when the board has
`caps.speech`, the owner has spoken replies on, and the device is not silent.
A backend that does not know the flag ignores it, as with `stream`.

**Reply field.** A backend that honours the flag adds one object:

```json
{"speech": {"url": "https://plugin.tiny.technology/media/<key>", "sha256": "<64 hex>", "duration_ms": 4200}}
```

as an SSE `data:` event before `[DONE]`, or as a top-level field next to
`text` in the classic JSON reply. The device plays it through the same path
as `speak <url> <sha256>`. The server chooses what to speak (the answer, or a
spoken summary of a long one) and keeps the clip under 30 s.

**Status.** `caps.speech` (bool), `speaking` (bool), and `speak_last` (the
`speak status` verdict string).

**Settings.** One row on the settings page, persisted in NVS
(`tinycfg/speakmode`): `off | voice asks | all asks`.

CHANGELOG and `TINY_GRAMMAR_VERSION` move to 19 with the first slice.

## Server side

The firmware cannot finish this alone. Somebody has to turn the answer text
into a WAV and put it in the media store:

- **tiny.technology backend** (not in this repo): honour `speak:"1"` in
  `/api/devices/ask`, synthesise, upload, and add `speech` to the reply. This
  is the complete path for every ask, including the AI button.
- **Relay-side, no backend change**: any owner-side client that already
  drives the device (the dashboard in `docs/dashboard/`, a tiny tool, a
  script) synthesises the text itself, uploads the WAV with the owner's
  Bearer to `/api/media` (`application/octet-stream`, a custom User-Agent:
  Cloudflare 403s urllib's), and sends `speak <url> <sha256>`. This covers
  asks that client made, not button asks answered by the backend.

## Slices

1. **S1, firmware, no server dependency.** `tiny_pcm` WAV parser + mono→stereo
   gain expander with host tests; PCM item kind in `audio_es8311`;
   `speak` verb; `caps.speech`; status fields; barge-in in the capture entry
   point; grammar v19. Proof on the FNK0104S bench: a reference WAV
   (`tools/speak_push.py`: upload + `speak`, same pattern as `ota_push.py`),
   `speak status` showing `done … 0 tx stalls`, and `miccheck` before and
   after reading quiet (the amp is off when it says it is).
2. **S2, firmware, waits on a server.** The ask opt-in, `speech` consumption
   on both reply shapes, and the settings row. Proof: a voice ask that is
   answered on the glass and out loud.
3. **S3, server.** One of the two paths above. Outside this repo for the
   backend; inside `docs/dashboard/` for the relay path.

## Open questions

- Which server path builds S3.
- The default `speakmode`: `voice asks` (answer in the mode the question
  came in), `off`, or `all asks`.
- Whether 30 s is the right cap, or the server should always send a short
  spoken summary and leave the full answer on the glass.
