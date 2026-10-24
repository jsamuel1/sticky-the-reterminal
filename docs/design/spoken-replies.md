---
readtime: 6
description: "Design for spoken agent replies on the codec boards: speech streamed on the ask connection, 16 kHz PCM, no length cap, played as it arrives."
---

# Spoken replies — design (grammar v19, proposed)

Status: **proposal, no code**. Third person. It covers the agent's reply audio
(TTS) on the boards that have a real speaker: today the FNK0104S, and the
AMOLED-1.8 once its ES8311 is proven. Nothing here changes the reTerminal: its
"speaker" is a piezo buzzer and stays chimes-only.

Owner decisions (2026-10-24):

- Speech is **streamed**, with **no length cap**. The device plays audio as it
  arrives. It never downloads a finished clip first.
- The **tiny.technology backend** synthesises speech inside `/api/devices/ask`.
- `speakmode` defaults to **voice asks**: a spoken question gets a spoken
  answer.

## What exists

- `audio_es8311` runs I2S0 full duplex at **16 kHz, 16-bit stereo slots**,
  4 × 240-frame DMA rings in internal RAM, and a PSRAM-stacked `audio_play`
  task that renders tone lists (`TinyNote`) and switches the SC8002B amp
  around silence (30 ms lead, 120 ms tail).
- `tiny_pcm` is the host-testable PCM layer (WAV header writer, tone
  renderer, stats).
- `sticky_buzzer_silent()` / `sticky_buzzer_soft()` are the owner's mute and
  volume tier, persisted in NVS.
- `POST /api/devices/ask` answers SSE (`data: {"t":…}` deltas, `{"card":…}`,
  `{"error":…}`, `[DONE]`) or classic JSON. The `stream:"1"` body flag is an
  opt-in that an older backend ignores. The device reads it with a 30 s
  per-read stall bound and a 1 KB line buffer, and holds `s_ask_mux` for the
  whole connection.
- Internal RAM is the scarce rail on the colour boards: one TLS session needs
  about 60 KB of it, and `tiny_upload` refuses below that.

## Format: raw PCM s16le, mono, 16 000 Hz

The I2S bus already runs at this rate, so the device needs no resampler and no
decoder. It is 32 KB/s raw, or about 43 KB/s base64 on the wire. A Wi-Fi link
that moves multi-megabyte OTA images carries that easily.

Opus is the obvious later change, because an unbounded stream makes bandwidth
matter more than it does for a clip. It costs a decoder component (flash, a
decoder stack, CPU while playing), so it waits until a measurement shows the
link cannot keep the ring fed. The wire format names its codec so that change
does not need a new event.

## Transport: on the ask connection

When the device sends `speak:"1"`, the backend interleaves audio events with
the text deltas on the SSE stream it already sends. The device keeps one TLS
session, as today.

```
data: {"t":"The forecast "}
event: audio
data: <base64 s16le, ≤ 3072 chars = 2304 B = 72 ms>
data: {"t":"is rain."}
event: audio
data: <base64 …>
…
data: {"text_done":true,"card":{…}}      ← glass finishes here
event: audio
data: <base64 …>                         ← the rest of the speech
data: [DONE]
```

- **`event: audio`**: the data is bare base64, not JSON, so the hot path
  runs no cJSON. A codec field rides once, at the start:
  `event: audio_fmt` / `data: {"codec":"pcm_s16le","rate":16000,"ch":1}`.
  The device refuses any other format with a named reason (`speech refused:
  codec opus`), then drops the remaining audio events and keeps the text.
- **`text_done`** is new. Text runs ahead of speech, so the final card would
  otherwise wait behind the audio tail. On `text_done` the device ends the
  glass stream (`tiny_display_stream_end`) and keeps reading audio until
  `[DONE]`. A backend that sends no audio sends no `text_done`, and `[DONE]`
  keeps its old meaning.
- **Line buffer.** Audio lines go to a separate 4 KB PSRAM line buffer, so the
  1 KB text line buffer is unchanged.

Rejected alternatives:

- **A second connection** for raw chunked PCM, opened from a URL in the first
  event. It avoids base64 and decouples text from audio, but it needs two TLS
  sessions at once, about 120 KB of internal RAM, which the colour boards do
  not have. Revisit only if a measured `internal_free` during an ask shows
  room for both.
- **A finished clip in the media store.** That is request/response, so it is
  ruled out by the owner decision above.

## Playback: a PSRAM ring with back-pressure

The ask task decodes base64 straight into a PSRAM ring. The player task
drains the ring to I2S, expanding mono to the stereo slots in 240-frame chunks
and applying the volume tier as a gain.

- **Ring**: `CONFIG_TINY_SPEAK_RING_KB`, default 1024, which holds about 32 s
  of speech.
- **Back-pressure.** When the ring is full, the ask task stops reading the
  socket. TCP flow control then holds the backend's writer, so length is
  unbounded and memory is not. The cost: once speech lags text by a full
  ring, the remaining text deltas wait with the audio. `text_done` normally
  lands well before that, because synthesis runs faster than real time and
  the backend sends text first.
- **Stall timer.** The 30 s per-read bound applies only while the ring has
  room. A device that is not reading because the ring is full is not stalled.
- **Prebuffer**: playback starts at 300 ms buffered, or at `[DONE]` if the
  stream ends sooner.
- **Underrun**: the player writes silence and keeps the amp on for up to 2 s,
  then turns the amp off and waits for the prebuffer again. Every underrun is
  counted in the verdict.
- **Mutex.** The ask, and `s_ask_mux` with it, lasts until `[DONE]` *and* the
  ring drains, so speech belongs to the ask that produced it.

Rules:

| situation | behaviour |
|---|---|
| owner muted (`silent`) | `speak:"1"` is not sent, so no audio crosses the link |
| volume tier soft | same audio, gain ÷ 8 (the chime ratio) |
| chime while speaking | dropped; the speech is the feedback |
| AI button or `voice` while speaking (barge-in) | stop the player, flush the ring, close the ask connection, append ` [speech stopped]` to the relay summary, release the mutex, amp off, then capture. The mic never records the speaker |
| `ask` (typed) or `miccheck` while speaking | the same barge-in |
| `speak stop` | the same, without starting a capture |
| any new card or `render_ui` | speech continues; audio and glass are independent |
| pocket lock | speech continues. Lock guards inputs, not output (same as `say`) |
| deep sleep requested | barge-in, then sleep |
| connection cut mid-speech | play what is buffered, verdict `cut after N s` |
| a bad base64 line | skipped and counted; the stream carries on (same as a torn text frame) |

## Capability: `caps.speech`, separate from `caps.speaker`

`caps.speaker` already means "a sound comes out", and on the reTerminal that
is a buzzer (the beacon's historical bit). A new `speech` field means "PCM
playback through a codec and an amp". FNK0104S: true. AMOLED-1.8: false until
its ES8311 is proven on hardware. reTerminal: false.

## Grammar v19 (proposed)

**Ask opt-in.** `POST /api/devices/ask` gains `speak:"1"` when the board has
`caps.speech`, the device is not silent, and `speakmode` covers this ask:
`voice asks` (the default) covers `voice` and the AI button, and `all asks`
adds typed `ask`. A backend that does not know the flag ignores it, as with
`stream`.

**Stream events.** `event: audio_fmt`, `event: audio`, and `text_done`, as
above.

**Verb `speak`**: relay-side control and the bench test vehicle.

```
speak stop              barge-in without capture
speak status            the last verdict
speak test [seconds]    a device-generated 440 Hz sweep pushed through the ring
                        and player, default 5 s (no network: proves the player,
                        the ring and underrun accounting on the bench)
```

`speak status` reports the way `play status` does:
`done 41.3 s, 0 underruns, 0 bad lines`, `stopped at 12.0 s (barge-in)`,
`cut after 8.2 s (connection closed)`, `refused: codec opus`.

The heartbeat `capabilities[]` lists `speak` only where `caps.speech` is
true, so the list stays exactly what dispatches. On the other boards the verb
answers `no speaker on this board (buzzer only)` instead of help text.

**Status.** `caps.speech` (bool), `speaking` (bool), `speak_last` (the
`speak status` verdict), `speakmode`.

**Settings.** One row on the settings page, persisted in NVS
(`tinycfg/speakmode`): `off | voice asks | all asks`, default `voice asks`.

CHANGELOG and `TINY_GRAMMAR_VERSION` move to 19 with the first slice.

## Backend contract (tiny.technology, not in this repo)

On `speak:"1"`:

1. Stream text exactly as today.
2. Feed the answer text to a streaming TTS as it is produced, and resample to
   16 kHz mono s16le if the TTS emits anything else.
3. Send `event: audio_fmt` once, then `event: audio` lines of ≤ 3072 base64
   characters, interleaved with the text deltas as the audio arrives.
4. Send `text_done` (with the final `card`, if any) when the text ends, keep
   sending audio, then send `[DONE]`.
5. Tolerate a slow reader. The device applies back-pressure, so a long
   answer holds the connection for about as long as it takes to speak.
   The route must not cap turn time at the text's length. It is already on
   Node, not Edge.
6. A client close (barge-in) cancels synthesis.

Without the flag, nothing changes.

## Slices

1. **S1, firmware, no backend dependency.** `tiny_pcm` gets a streaming
   base64 decoder, the mono→stereo gain expander and the ring index math, all
   with host tests. `audio_es8311` gets the ring player with prebuffer and
   underrun handling. Also: `speak stop|status|test`, `caps.speech`, the
   status fields, and barge-in at the capture entry point; grammar v19.
   Proof on the FNK0104S bench: `speak test 60` reaches `done 60.0 s,
   0 underruns`, `miccheck` reads quiet afterwards (the amp is really off),
   and the AI button pressed mid-test stops it.
2. **S2, firmware, against the backend contract.** The ask opt-in, the SSE
   audio and `text_done` handling, back-pressure, and the settings row. A
   host test feeds a recorded SSE transcript through the parser. Proof: a
   voice ask longer than the ring is answered on the glass and out loud
   end to end.
3. **S3, backend.** The contract above, built by the tiny.technology backend
   owner.

## Open questions

- Ring size: 1 MB (about 32 s of text lead) or more. PSRAM is 8 MB, but the
  canvas, frame prefetch and the ask buffers already live there; measure the
  free PSRAM during a `play` before raising it.
- Whether `speakmode` should include `all asks` at all, or only `off` and
  `voice asks`.
