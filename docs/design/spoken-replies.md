---
readtime: 13
description: "Design for spoken agent replies on the codec boards: speech streamed on the ask connection, 16 kHz PCM, no length cap, played as it arrives."
---

# Spoken replies — design (grammar v20, proposed)

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
the text deltas on the SSE stream it already sends. Audio adds no second
connection: text, card and speech share one ask response. This is not a claim
that the whole firmware has only one socket — a physical-button ask can already
overlap the node task's short heartbeat or relay poll. Every event is a
complete SSE block: one `data:` field and a blank line. The blank lines below
are wire bytes, not formatting space.

```
data: {"t":"The forecast "}

event: audio_fmt
data: {"codec":"pcm_s16le","rate":16000,"ch":1}

event: audio
data: <base64 s16le, ≤ 3072 chars = 2304 B = 72 ms>

data: {"t":"is rain."}

event: text_done
data: {"text_done":true,"card":{…}}      ← glass finishes here

event: audio
data: <base64 …>                         ← the rest of the speech

data: [DONE]

```

The parser changes from today's data-line shortcut to an SSE block state
machine:

- It accepts LF, CRLF or a lone CR as a line end, and it is fed raw read
  chunks whose boundaries may fall anywhere: inside a field name, inside the
  base64, or between the CR and LF of one CRLF. A CR that ends a chunk sets a
  pending flag, and an LF that opens the next chunk is swallowed rather than
  read as a blank line (which would dispatch a block early). It accumulates
  `event:` and `data:` fields until the blank-line dispatch boundary, then
  resets event type, data length, field count and poison flag to the default
  `message` state, whether the block was accepted, rejected or ignored. Field
  order inside a block is irrelevant. Comment lines and unknown `id:` /
  `retry:` fields are ignored. The parser is a host-testable C module
  (`tiny_sse`) with no ESP-IDF dependency, the `tiny_pcm` pattern.
- This protocol emits exactly one `data:` field per block. A second `data:`
  field, an overlong physical line, or EOF before the blank boundary drops the
  **whole block** and increments `bad_lines`; no retained prefix is parsed.
  Unknown event types are dropped as one block.
- A 4 KB PSRAM field buffer admits the largest supported wire line
  (`data: ` + 3072 base64 characters). Default `message` data keeps today's
  1 KB limit. Limits include field prefixes and are checked before a byte is
  appended.
- `audio_fmt` must occur exactly once before the first `audio` block and must
  equal `pcm_s16le`, 16000 Hz, one channel. Audio before it or a duplicate or
  changed format ends speech for this ask. Before any sample plays the verdict
  is `refused`; after playback starts it is `cut after N s`; either path flushes
  unplayed bytes and text keeps streaming.
- Each audio line is validated as canonical base64 and decoded into a bounded
  2304-byte scratch buffer first. Standard final-quartet padding is accepted;
  misplaced/excess padding is not. Only a completely valid, even-byte PCM
  chunk is committed atomically to the ring, including across ring wrap. Bad
  base64 or an odd decoded byte count drops that whole block and increments
  `bad_lines`; later audio may continue, with no partially written sample.
- `audio_error` carries one JSON object with a client-safe `code` from
  `provider | timeout | cancelled | internal`; raw provider errors never cross
  this boundary. Before any sample plays it publishes `refused: <code>` and
  flushes buffered audio; after playback starts it publishes
  `cut after N s (tts: <code>)` and drains already-buffered valid audio. Later
  audio blocks are ignored. Either way, valid text remains an agent answer
  rather than becoming an agent error. If the backend ignores `speak:"1"` and
  sends no audio event at all, `[DONE]` records `not offered by backend`, not
  `done 0.0 s`.

Older firmware ignores `event:` fields, ignores the new JSON objects, fails
closed on bare base64, and still finishes on `[DONE]`. A backend that sends no
audio sends neither `audio_fmt` nor `text_done`; `[DONE]` keeps its old
meaning.

**`text_done`** is new. Text runs ahead of speech, so the final card would
otherwise wait behind the audio tail. The first accepted `text_done` is the
glass's end of stream, and it runs exactly once:

- It calls `tiny_display_stream_end` with its own `card`, if any, or with a
  `card` held from an earlier block of this ask. The ask then frees that
  pointer and keeps no final-card ownership after this point.
- The ask moves to an audio-only phase. Later `t`, `card` and `error`
  objects are counted and ignored, and so is a duplicate `text_done`. Nothing
  more is appended to the glass or the relay summary.
- The later `[DONE]` only ends the audio producer. It does not call
  `tiny_display_stream_end` again and does not render a card. A `render_ui`,
  relay card or page change that lands during the audio tail therefore stays
  on the glass; the old answer never overwrites it.
- A cut, refusal or cancellation after `text_done` changes only the speech
  verdict. The glass already shows the finished answer, so no ` [stream cut]`
  is appended to it. A cut **before** `text_done` keeps today's cut handling.

The final card stays in its data object, so old firmware keeps rendering it
at `[DONE]` as it does today.

Rejected alternatives:

- **A dedicated second connection** for raw chunked PCM, opened from a URL in
  the first event. It avoids base64 and decouples text from audio, but creates
  two long-lived TLS sessions (ask plus audio). A physical-button ask can
  already overlap one short relay poll, so this alternative raises the peak
  from two clients to three and adds another roughly 60 KB demand on scarce
  internal RAM. S2 retains one long-lived ask connection, measures the existing
  two-client peak, and adds a speech-admission floor: below it, the device still
  asks but omits `speak:"1"` and records `speech refused: internal RAM`. No
  separate audio socket is opened.
- **A finished clip in the media store.** That is request/response, so it is
  ruled out by the owner decision above.

## Playback: a PSRAM ring with back-pressure

The ask task decodes base64 straight into a PSRAM ring. The player task
drains the ring to I2S, expanding mono to the stereo slots in 240-frame chunks
and applying the volume tier as a gain.

- **Ring**: `CONFIG_TINY_SPEAK_RING_KB`, default 1024, which holds about 32 s
  of speech. It is a Kconfig `choice` of 256, 512, 1024 or 2048 KB, so only a
  power of two can be configured. A `static_assert` that the byte size is a
  nonzero power of two, at least 64 KB, backs the index mask in code. It is
  allocated from PSRAM when the codec player starts. Allocation
  failure leaves static `caps.speech` as hardware truth but makes player
  readiness false, suppresses `speak:"1"`, and reports `speech refused: no
  PSRAM`; no null ring reaches the parser.
- **Back-pressure.** When the ring is full, the ask task stops reading the
  socket. TCP flow control then holds the backend's writer, so length is
  unbounded and memory is not. The cost: once speech lags text by a full
  ring, the remaining text deltas wait with the audio. `text_done` normally
  lands well before that, because synthesis runs faster than real time and
  the backend sends text first.
- **Stall timer.** The 30 s per-read bound applies only while the ring has
  room. A device that is not reading because the ring is full is not stalled.
- **Prebuffer**: while the producer is open, playback starts or resumes at
  300 ms buffered. The producer publishes one terminal state — `done`, `cut`,
  `refused`, or `cancelled`. `done` and `cut` bypass the threshold and drain
  every remaining complete sample; `refused` and `cancelled` flush it. This
  prevents a short final tail from waiting forever below the prebuffer
  threshold and prevents bytes accepted before a late format violation from
  playing under a refused stream.
- **Underrun**: while the producer is open, the player writes silence and
  keeps the amp on for up to 2 s, then turns the amp off and waits for the
  prebuffer again. Every underrun is counted in the verdict. Producer terminal
  state, not a byte-count guess, wakes that wait.
- **Ownership.** One player task owns I2S TX and the amp. Its command queue
  carries tone, speech-start and speech-stop generations. It acknowledges
  speech idle only after residual drain or flush, tail silence and amp-off.
  Chime enqueue checks the same speech generation, so a race cannot put a
  chime behind speech after the drop decision. Played/received sample and
  error counters are saturating `uint64_t`; an intentionally unbounded stream
  cannot wrap a verdict or duration counter.
- **Mutex.** The ask worker, and `s_ask_mux` with it, lasts until the producer
  has published its terminal state (`done`, `cut`, `refused` or `cancelled`)
  and the player has sent its idle acknowledgement, so speech belongs to the ask that
  produced it. No producer holds a ring lock while blocking on I/O or waiting
  for free space.

## Cancellation and task ownership

Barge-in is a request to owners, never cross-task resource destruction. A
small speech controller registers exactly one active producer generation and
its cancellation sink under a `portMUX_TYPE` critical section; controls copy
that generation and signal its owner with a task notification outside the
critical section. Network speech registers the ask worker; `speak test`
registers its test-producer task. The owner rechecks the generation before
acting, so a stale notification cannot stop a newer utterance:

1. One dedicated ask worker owns `s_ask_mux`, the active
   `esp_http_client_handle_t`, SSE/parser state, ring-producer state and the
   completion of the current relay envelope. Relay `ask` / `voice` copy their
   envelope id and request into its bounded queue and defer the reply to this
   worker instead of blocking `node_task`; physical-button asks and the offline
   ask-queue drain use the same owner. While speech is active, one pending
   replacement is retained; additional asks receive an explicit busy reply.
   Before the first audio sample, replacement asks retain today's behavior and
   are refused as `ask already in flight` rather than cancelling an agent turn
   that has not begun speaking. Completion is posted back to the node task so
   an offline question is popped only under today's delivered-or-backend-spoke
   rule. For a relay request, the worker emits one completion record;
   `node_task` alone sends its reply PATCH. The completion queue cannot drop:
   it is sized to the admitted ask plus replacement and the worker waits for
   space. Physical asks have no envelope. An explicit `drain_inflight`
   generation prevents a persisted offline question from being submitted
   again before its completion carries the pop-or-retain decision. The node
   task keeps relay polling at the active cadence while speech runs; heartbeat
   and event posts remain best-effort short overlaps as today. Speech admission
   requires the measured internal-RAM floor for ask plus one short control
   client. Below it, the ask still runs without `speak:"1"`; this feature never
   turns memory pressure into loss of text or remote reach.
2. A control path increments the active cancellation generation and signals
   its registered producer. It never calls `esp_http_client_close`,
   `esp_http_client_cancel_request`, releases `s_ask_mux`, mutates parser
   state or drives the amp. ESP-IDF does not document one client handle as
   safe for concurrent task use. The network producer's full-ring wait is on
   **space-or-cancel**, so the same signal wakes it immediately; it never has
   to drain a 32 s ring before noticing barge-in. The test producer uses the
   same signal and terminal-state path without owning an HTTP client.
3. The owning read loop checks cancellation between bounded socket reads. Its
   transport seam must return `data`, `idle` or `cut`: a 100 ms idle result
   continues until today's 30 s no-byte deadline; EOF or another terminal
   read error is a cut. This avoids assuming every negative
   `esp_http_client_read()` result is a recoverable timeout. On cancellation
   the owner closes and cleans up the client, publishes `cancelled` to the
   player, waits for its idle acknowledgement, and emits the old request's
   completion with ` [speech stopped]`. Its RAII guard then releases
   `s_ask_mux` before it starts any queued replacement. `node_task` sends the
   deferred relay PATCH; no task may capture until the player's acknowledgement.
4. AI-button and relay `ask` / `voice` barge-in use cancel-and-replace; the
   button callback only queues the replacement and never waits. Remote stop
   latency is bounded by the forced-active relay cadence (5 s); the physical
   button signals locally. `miccheck`, `speak stop` and `sleep` signal then wait
   on the same bounded idle barrier. A barrier timeout returns `speech stop
   timed out` and does not capture or sleep with the amp active. Repeated stop
   and a stop racing natural EOF are idempotent: one generation completes once,
   one old relay reply is sent once, and the newer request cannot inherit old
   bytes.

Rules:

| situation | behaviour |
|---|---|
| owner muted before an ask | `speak:"1"` is not sent, so no audio crosses the link |
| owner switches to silent while speaking | signal the active generation; player flushes and turns the amp off. Unmuting does not resume discarded audio |
| volume tier soft or changed mid-speech | gain ÷ 8; tier is sampled per output chunk, so the next chunk reflects the new setting without rewriting ring bytes |
| chime while speaking | dropped; the speech is the feedback |
| AI button or `voice` while speaking (barge-in) | signal cancel-and-replace; the ask worker closes its connection, the player flushes and acknowledges amp-off, then the replacement captures. The mic never records the speaker |
| `ask` (typed) while speaking | signal cancel-and-replace; start the new ask only after old worker/player quiescence |
| `miccheck` while speaking | signal cancellation, wait for amp-off acknowledgement, then capture |
| `speak stop` | cancel active speech without capture; if no speech is active, reply `not speaking` and do not cancel an ask that is still generating text |
| `speak test [seconds]` | honour silent mode; refuse while an ask is in flight but has not begun speaking; while speech is active, cancel it, await amp-off, then register the test generation |
| any new card or `render_ui` | speech continues; audio and glass are independent |
| pocket lock | speech continues. Lock guards inputs, not output (same as `say`) |
| deep sleep, auto-sleep, OTA reboot or node shutdown | signal cancellation, wait for the bounded player-idle/amp-off barrier, then cross the power boundary; on timeout refuse a requested sleep/OTA, while shutdown's last resort forces the amp GPIO off before teardown |
| connection cut or 30 s stall mid-speech | terminal state `cut`; the buffered residual drains even below the prebuffer; verdict `cut after N s` |
| a malformed audio block | dropped whole, the ring unchanged, counted in `bad_lines`; the stream carries on |
| a malformed text line | skipped as a torn frame, as today |
| text, card or error after `text_done` | counted and ignored; the glass is not touched again |

## Capability: `caps.speech`, separate from `caps.speaker`

`caps.speaker` already means "a sound comes out", and on the reTerminal that
is a buzzer (the beacon's historical bit). A new `speech` field means "PCM
playback through a codec and an amp". FNK0104S: true. AMOLED-1.8: false until
its ES8311 is proven on hardware. reTerminal: false.

## Grammar v20 (proposed)

The main branch is already at grammar 19, which is strict direct-artifact OTA
arguments. Spoken replies are the next grammar change, v20.

**Ask opt-in.** Each queued request carries an explicit origin:
`physical_voice`, `relay_voice`, `typed`, or `offline_typed`; mode decisions do
not infer origin from mutable buffers. `POST /api/devices/ask` gains
`speak:"1"` when the board has `caps.speech`, `speech_ready` is true, the
measured heap admission passes, the device is not silent, and `speakmode`
covers this ask: `voice asks` (the default) covers both voice origins, and
`all asks` adds both typed origins. A backend that does not know the flag
ignores it, as with `stream`.

**Stream events.** `event: audio_fmt`, `event: audio`,
`event: audio_error`, and `event: text_done`, as above.

**Verb `speak`**: relay-side control and the bench test vehicle.

```
speak stop              barge-in without capture
speak status            the last verdict
speak test [seconds]    a device-generated 440 Hz tone pushed through the ring
                        and player by a cancellable test-producer task; default
                        5 s, range 1..300 s; returns `test started` immediately
                        (no network: proves player, ring and accounting)
```

`speak status` reports the way `play status` does:
`done 41.3 s, 0 underruns, 0 bad lines`, `stopped at 12.0 s (barge-in)`,
`cut after 8.2 s (connection closed)`, `refused: codec opus`.

The heartbeat `capabilities[]` lists `speak` only where `caps.speech` is
true, so the list stays exactly what dispatches. On the other boards the verb
answers `no speaker on this board (buzzer only)` instead of help text.

**Status.** `caps.speech` (hardware bool), `speech_ready` (player/ring bool),
`speaking` (bool), `speak_last` (the `speak status` verdict), and `speakmode`.

**Settings.** One row on the settings page, persisted in NVS
(`tinycfg/speakmode`): `off | voice asks | all asks`, default `voice asks`.

CHANGELOG and `TINY_GRAMMAR_VERSION` move to 20 only in the firmware release
that contains **both S1 and S2**. The slices are implementation order, not
separately shippable grammar states: S1 may be bench-tested from a development
build, but no released v20 may advertise `speak`, `caps.speech`, status fields
or the request/events without the complete S2 parser, cancellation and
settings contract.

## Backend contract (tiny.technology, not in this repo)

On `speak:"1"`:

1. Stream text exactly as today, one default `message` SSE block per delta.
2. Feed the answer text to a streaming TTS as it is produced, and resample to
   16 kHz mono s16le if the TTS emits anything else.
3. Send one `audio_fmt` block before any audio, then `audio` blocks of no more
   than 3072 canonical-base64 characters, interleaved with text as audio
   arrives. End every block with a blank line. On synthesis failure after
   response start, send one `audio_error` block with a client-safe code from
   the closed alphabet above, continue/finish valid text, then send `[DONE]`.
4. Send one `text_done` block (with the final `card`, if any) when text ends,
   keep sending audio, then send `[DONE]` as a default `message` block.
5. Tolerate a slow reader. The device applies back-pressure, so a long
   answer holds the connection for about as long as it takes to speak.
   The route must not cap turn time at the text's length. It is already on
   Node, not Edge.
6. A client close (barge-in) cancels synthesis and releases its generator;
   no detached TTS job may keep producing an unobserved unlimited stream.

Without the flag, nothing changes.

## Slices

1. **S1, firmware, no backend dependency; not released alone.** `tiny_pcm`
   gets the canonical base64 validator/decoder, the mono→stereo gain expander
   and the ring. A pure-C speech controller (generation, terminal state,
   cancellation sink) and a player model get injected clocks, a fake I2S sink
   and a fake amp pin so they run on the host. `audio_es8311` gets the ring
   player with prebuffer, underrun, residual drain, flush and the amp-off
   acknowledgement. Also: `speak stop|status|test`, `caps.speech`, status
   fields and barge-in at the capture entry point, behind a development-build
   flag (grammar stays 19 until S2; see above). S1 host tests, each pinning
   one contract:
    - Ring: whole commits straddling the buffer end at every offset of a small
      ring read back byte for byte; a commit without room for the whole chunk
      is refused and leaves the write index unchanged; index wrap keeps fill
      correct; flush empties it.
    - Player model, ticked: prebuffer holds output to 300 ms; an open-stream
      gap gives one underrun, 2 s of amp-on silence, amp off, fresh prebuffer;
      `done`/`cut` with 50 ms buffered, and `done` arriving while the player
      waits after an underrun below the threshold, both drain to empty,
      zero-pad the last chunk, write the tail, turn the amp off and
      acknowledge; `refused`/`cancelled` flush within one DMA ring. No run
      ends with bytes in the ring or the amp on.
    - Amp ownership: a chime enqueued at every point of a session's life
      (before arm, mid-speech, racing the stop, after idle ack) is either
      played after idle ack or dropped and counted, never interleaved with
      speech; a capture request is admitted only after the idle ack.
    - Controller: a stale-generation stop, a double stop, and a stop racing
      natural EOF each complete the generation exactly once.

   Proof on the FNK0104S bench (`speak test` replies `test started`; the
   verdict is read with `speak status`):
    - `speak test 60` reaches `done 60.0 s, 0 underruns`, and `miccheck`
      reads the room floor afterwards (the amp is really off).
    - A 1 s test, which is longer than the prebuffer but proves nothing about
      the tail, and a test whose producer is paused halfway to force an
      underrun followed by a sub-prebuffer tail, both end `done` with the amp
      off.
    - The AI button pressed mid-test stops it, and capture starts only after
      the log's amp-off acknowledgement.
    - Relay `speak stop` sent twice gives the verdict, then `not speaking`.
    - A chiming `render_ui` mid-test is dropped and counted.
2. **S2, firmware, against the backend contract.** The dedicated ask worker,
   deferred relay replies and offline-drain completion, ask opt-in, SSE block
   parser, audio and `text_done` handling, back-pressure, cancellation barrier,
   measured two-client speech admission, player-allocation failure, and the
   settings row. Host tests feed recorded SSE through arbitrary byte
   fragmentation and deterministic fake transport/player seams. They cover
   LF/CRLF and blank dispatch; field reordering; overlong and unterminated
   blocks; unknown/duplicate events; valid-final-padded base64 plus invalid
   alphabet/padding; odd-byte and ring-wrapping chunks; pre- and post-playback
   `audio_error`; ignored audio after terminal error; invalid producer;
   stale-generation stop; player-allocation failure; full-ring back-pressure;
   open-stream underrun; EOF with less than one prebuffer; natural EOF racing
   stop; repeated stop; pre-audio busy refusal; a stalled producer (fake
   transport returns `idle` for 30 s with ring room → `cut`, residual drains,
   epilogue runs) versus a full ring held 60 s with no read (not a stall);
   a cancel arriving while the producer waits for ring space (wakes at once);
   `text_done` ending the glass exactly once, later `t`/`card`/duplicate
   `text_done` ignored, `[DONE]` and a post-`text_done` cut leaving the glass
   untouched, and a fake `render_ui` card painted during the audio tail still
   current at `[DONE]` (no replay of the old final card);
   button and relay replacement asks; no double reply; and no capture before
   amp-off. Auto-sleep tests pin that ask-worker or speech activity blocks the
   due path; explicit sleep/OTA tests pin cancellation-before-power transition
   and timeout refusal. Shutdown tests pin amp-off even when normal
   acknowledgement fails. Mutation controls that remove event reset, atomic
   ring commit, terminal-tail wakeup, generation checks, async-ask sleep guard
   or power-boundary barrier must fail. Proof on hardware: a voice ask longer
   than the ring is answered on the glass and out loud end to end; mid-answer
   AI-button barge-in begins capture only after amp-off; `miccheck` then reads
   quiet.
3. **S3, backend.** The contract above, built by the tiny.technology backend
   owner. Its integration test streams more than one ring, throttles the
   reader, then disconnects and proves synthesis is cancelled. A recorded
   backend transcript is the S2 firmware fixture, so both sides test the same
   bytes.

Repository validation for each firmware slice: `tests/run.sh`, all three
board builds (`tools/idf.sh build`, `tools/build_board.sh fnk0104s`, and
`tools/build_board.sh amoled18`), then the FNK0104S bench proof above. The
design-only change runs `STICKY_SKIP_FW=1 mkdocs build --strict`; its four
existing install-manifest warnings are compared with `origin/main` rather
than reported as a green strict build.

## Open questions

- Speech admission floor: derive it from repeated worst-case measurements of
  ask plus one short relay/heartbeat client, then add explicit margin. Until
  that floor is measured on FNK0104S, `speech_ready` stays false in release
  builds; do not ship a guessed threshold that can turn allocator pressure into
  a reboot.
- Ring size: 1 MB (about 32 s of text lead) or more. PSRAM is 8 MB, but the
  canvas, frame prefetch and the ask buffers already live there; measure the
  free PSRAM during a `play` before raising it.
- Whether `speakmode` should include `all asks` at all, or only `off` and
  `voice asks`.
