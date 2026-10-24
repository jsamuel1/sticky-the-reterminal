---
readtime: 18
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

### Glass takeover before `text_done`

Today any `tiny_display_render_card` call while the text stream is on resets
the stream ("render during active stream — stream ended by new card"), and the
ask task keeps calling `stream_append` / `commit` / `end` against a stream
that no longer exists. Every glass-claiming path goes through that one choke
point: relay `render_ui`, `say`, `page`, the `messages` and `sensors` cards,
the home and page buttons, the auth cards and the sleep card. With speech the
ask lives longer, so the rule becomes explicit:

- `tiny_display_stream_begin` returns a **stream token** (a nonzero `uint32_t`
  epoch). `stream_append`, `stream_commit`, `stream_note` and `stream_end`
  take that token. Stream state and the token compare sit under the existing
  `s_display_mux`, so a reset by another task and an append by the ask worker
  cannot interleave. The home re-begin used by voice (listening frame, then
  the SSE begin) keeps the same token.
- A takeover is any `render_card` that resets an active stream. It retires
  the token. Later calls with that token return `ESP_ERR_INVALID_STATE` and
  touch nothing on the glass.
- On the first stale return the ask marks its glass sink **detached**. The
  agent turn is not cancelled and speech is not cancelled; the question still
  gets its answer. Remaining `t` deltas still go into the relay-reply summary
  and `s_last_answer`, so the remote caller and the home preview keep the full
  text. The glass does not get them.
- A detached ask never paints again. A final card carried by `text_done`, an
  earlier held card, or (old backend) the card at `[DONE]` is freed without
  rendering. A cut or `error` after takeover adds no ` [stream cut]` to
  another card. The relay summary gets ` [glass taken by <card_id>]` once.
- After `text_done` the ask holds no token, so a takeover in the audio tail
  is just an ordinary card, as described above.
- A barge-in cancel or `speak stop` before `text_done` ends the ask's own
  stream, if its token is still valid, with `stream_end(NULL)` on the text so
  far plus ` [stopped]`. This is the counterpart of today's ` [stream cut]`.
  A held card is freed unrendered. A replacement ask then begins its own
  stream.
- `ask` while an ask is in flight keeps today's refusal before the first audio
  sample, and cancel-and-replace after it (see below). A replacement begins a
  new stream and a new token only after the old ask's epilogue, so two asks
  never share a token.

The classic (non-SSE) JSON path has no stream, so it is unchanged: it renders
its answer when the body arrives, as today.

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

Barge-in is a request to owners, never cross-task resource destruction.

**Signal lifetime.** No control path ever holds a pointer, task handle or
callback that belongs to a producer. The speech controller owns one static
event group, created at boot and never deleted, plus three generation words
(`active_gen`, `cancel_gen`, `ack_gen`) guarded by a `portMUX_TYPE`:

- A producer registers by setting `active_gen` under the lock and clearing
  `CANCEL_BIT`. It unregisters by setting `active_gen = 0` under the lock.
  Registration never stores a task handle or a sink.
- A control reads `active_gen` under the lock. If it is zero the control
  answers `not speaking`. Otherwise it sets `cancel_gen = active_gen`, leaves
  the lock, and calls `xEventGroupSetBits(CANCEL_BIT)` on the static group.
  Every caller (the button callback in the esp_timer task, `node_task`, the
  sleep path) is a task, never an ISR.
- The producer's full-ring wait is `xEventGroupWaitBits(SPACE_BIT |
  CANCEL_BIT)`. On wake it compares `cancel_gen` with its own generation
  under the lock. A match means cancel; anything else clears the stale bit
  and continues.
- The player acknowledges idle the same way: it writes `ack_gen` under the
  lock and sets `IDLE_BIT`. Waiters compare `ack_gen` with the generation they
  stopped.

If a producer finishes and unregisters between a control's lookup and its
`SetBits`, the bit lands on an object that outlives every producer. The next
producer either sees `cancel_gen` belong to another generation and ignores
it, or clears the bit when it registers. Nothing can dangle. The other
cross-task targets are permanent too: the ask worker and the player are
created at boot and never deleted, and the replacement queue and completion
queue are static. The only short-lived task, the `speak test` producer, is
never signalled directly.

Network speech registers the ask worker; `speak test` registers its
test-producer task:

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
2. A control path follows the signal-lifetime rule above: it sets
   `cancel_gen` and the static `CANCEL_BIT`, and touches nothing the producer
   owns. It never calls `esp_http_client_close`,
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
   button callback only queues the replacement and never waits. A hold-to-talk
   replacement carries its gesture, which the next section defines. Remote stop
   latency is bounded by the forced-active relay cadence (5 s); the physical
   button signals locally. `miccheck`, `speak stop` and `sleep` signal then wait
   on the same bounded idle barrier. A barrier timeout returns `speech stop
   timed out` and does not capture or sleep with the amp active. Repeated stop
   and a stop racing natural EOF are idempotent: one generation completes once,
   one old relay reply is sent once, and the newer request cannot inherit old
   bytes.

### Hold-to-talk across barge-in

On the colour boards, BOOT hold arms capture at the 450 ms long-press and
release ends it. Today release sets a global `s_talk_released`, and capture
reads it as soon as it starts. With speech playing, capture can only start
after the player's amp-off acknowledgement, and the user may let go of BOOT
before that. The global flag would then give a clipped 0.5 s capture of
nothing, sent as a real voice ask. So the gesture gets a record that it owns:

- `on_talk_start` allocates a **gesture generation** `G` and writes
  `{G, pressed_at, released = false}` under the gesture `portMUX`. A newer
  press replaces the record, and an older generation can never consume it.
  `on_talk_release` sets `released` and `released_at` only when the record
  still holds the generation that was pressed. Every path uses this record,
  with or without speech, so the gesture has one meaning.
- The voice request queued to the ask worker carries `G`. Once the player
  acknowledges idle, the worker takes the record at **capture eligibility**
  `E`, which is when the acknowledgement arrives:
    - **Released before `E`** — the gesture was a stop. Speech ends with
      `stopped at N s (barge-in)`, no capture runs and no ask is sent. The
      glass keeps the old answer, ended by the cancellation rule in "Glass
      takeover". The press-then-let-go gesture people already use to
      interrupt therefore works.
    - **Still held at `E`** — capture starts at `E` and runs until release.
      The 0.5 s floor and the 15 s cap count from `E`, not from the press.
    - **Released after capture starts** — today's rule: stop at release,
      but not before 0.5 s of frames.
- The 3 s arm TTL also counts from `E`, so a bounded stop barrier cannot
  expire an arm the user is still holding. A barrier timeout drops the
  gesture with `speech stop timed out` on the status line. The dead-end
  double beep stays silent, because the player that would play it is the
  one that did not go idle.
- The pocket lock is still checked at the capture entry point, so a lock
  that engages during the barrier refuses the capture as today.
- Relay `voice <seconds>` and the reTerminal fixed-length click carry a
  duration and no gesture, so they are unaffected.

Rules:

| situation | behaviour |
|---|---|
| owner muted before an ask | `speak:"1"` is not sent, so no audio crosses the link |
| owner switches to silent while speaking | signal the active generation; player flushes and turns the amp off. Unmuting does not resume discarded audio |
| volume tier soft or changed mid-speech | gain ÷ 8; tier is sampled per output chunk, so the next chunk reflects the new setting without rewriting ring bytes |
| chime while speaking | dropped; the speech is the feedback |
| AI button or `voice` while speaking (barge-in) | signal cancel-and-replace; the ask worker closes its connection, the player flushes and acknowledges amp-off, then the replacement captures. The mic never records the speaker. BOOT released before amp-off = stop only, no ask; still held = capture from amp-off to release (see hold-to-talk above) |
| `ask` (typed) while speaking | signal cancel-and-replace; start the new ask only after old worker/player quiescence |
| `miccheck` while speaking | signal cancellation, wait for amp-off acknowledgement, then capture |
| `speak stop` | cancel active speech without capture; if no speech is active, reply `not speaking` and do not cancel an ask that is still generating text |
| `speak test [seconds]` | honour silent mode; refuse while an ask is in flight but has not begun speaking; while speech is active, cancel it, await amp-off, then register the test generation |
| any new card or `render_ui` **before** `text_done` | the card takes the glass and retires the stream token; the ask detaches its glass sink, so text goes on into the relay summary only and no final card is painted. Speech continues |
| any new card or `render_ui` **after** `text_done` | an ordinary card; speech continues. The ask holds no glass token |
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

`caps.speech` is a `board_caps` field reported in `status`. It is **not** a
filter on the verb roster. `s_caps[]` in `tiny_node.cpp` is one static list,
identical on every board. The heartbeat, `pair/start`, the BLE tool catalog
and the provisioning tools (`provision_lib.py` and `provision.js` parse it from
source) all consume it. A runtime filter would make the heartbeat disagree
with the claim the provisioning tools make from that source. This design adds
no TinyBeacon capability bit.

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

`speak` joins `s_caps[]` unconditionally, in the same commit that adds its
dispatch branch, with no Kconfig or board `#if` around either. It therefore
dispatches on every board, and "advertise exactly what dispatches" holds. On
a board with `caps.speech` false, every subcommand returns a typed refusal,
`speak refused: no speech playback on this board (buzzer only)`, rather than
unknown-verb help text. That is the precedent `ota` set: it is listed on
every board and answers `OTA refused: OTA is disabled on this board` where
`caps.ota` is false. A caller learns whether speech works from `status`
`caps.speech` / `speech_ready`, not from the roster. The static
`provision.js` copies gain `speak` in the same commit; `selftest.py` and
`provision_host_test.sh` already fail on a mismatch. The `commands.md` verb
count moves from 23 to 24.

**Status.** `caps.speech` (hardware bool), `speech_ready` (player/ring bool),
`speaking` (bool), `speak_last` (the `speak status` verdict), and `speakmode`.

**Settings.** One row on the settings page, persisted in NVS
(`tinycfg/speakmode`): `off | voice asks | all asks`, default `voice asks`.

CHANGELOG and `TINY_GRAMMAR_VERSION` move to 20 only in the firmware release
that contains **both S1 and S2**. The slices are implementation order, not
separately shippable grammar states: S1 may be bench-tested from a development
build, but no released v20 may advertise `speak`, `caps.speech`, status fields
or the request/events without the complete S2 parser, cancellation and
settings contract. S1 may still add `speak` to `s_caps[]` together with its
dispatch branch, because the roster rule is about dispatch, not grammar. A
build between the S1 and S2 commits is a development build and is not
tagged.

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

## Compared with the LLM request

The owner asked how speech compares with the LLM request. The device sends
one request; the backend runs two generators behind it: the agent's model
first, then TTS on the model's text.

| | LLM text (today) | TTS speech (this design) |
|---|---|---|
| Device request | `POST /api/devices/ask` with `text` or `audioUrl`, `tiny`, `stream:"1"` | the same request with `speak:"1"` added. No second request, URL or socket |
| What runs | one owner-scoped agent turn: model, tools, memory | the same turn, unchanged. TTS reads the turn's output text and never changes it |
| Who picks model / provider | backend, from the owner's agent (`tiny` slug) | backend: TTS provider, model and voice are backend configuration. The device sends no provider, model or voice field |
| Wire | default `message` blocks `{"t"}`, `{"card"}`, `{"error"}`, `[DONE]` | the same blocks, plus `audio_fmt`, `audio`, `audio_error`, `text_done` |
| Output size | bounded: an 8 KB glass buffer and an 8000 B relay reply | unbounded: about 32 KB/s of PCM, bounded in memory only by the ring |
| Duration | ends when the model finishes | ends when the speech finishes, usually well after `text_done`, so the connection stays open for the audio tail |
| Flow control | the device reads as fast as bytes arrive | ring full → the device stops reading → TCP holds the backend's TTS writer |
| Stall rule | 30 s with no byte is a cut | the same, but only while the ring has room |
| Failure | `error` is shown as an agent error | `audio_error` changes only the speech verdict; the text stays an answer |
| Concurrency | a second ask is refused while one is in flight | the same before the first audio sample; after it, barge-in cancels and replaces |
| Cancellation | none today; the turn runs to `[DONE]` | the device closes the connection; the backend must stop TTS (contract item 6). Tool side effects already done are not rolled back |
| Glass | typed live, then the final card | unchanged up to `text_done`; audio never touches the glass |
| Off switch | none | `speakmode`, silent mode, `speech_ready` and heap admission. Any one of them omits `speak:"1"` and leaves the request byte-identical to today |

What stays the same: one turn, one text, one card, one transcript
(`/api/devices/transcript` records the text, not the audio), one request
shape. What changes is the lifetime of the response stream: it now follows
playback rather than generation, so back-pressure, the stall rule and
cancellation become device-visible. Voice per agent, TTS cost and any storage
of synthesized audio are backend decisions outside this repo. The input side
has the same split today: the device uploads WAV, and the backend chooses the
speech-to-text.

## Slices

1. **S1, firmware, no backend dependency; not released alone.** `tiny_pcm`
   gets the canonical base64 validator/decoder, the mono→stereo gain expander
   and the ring. A pure-C speech controller (generations, terminal state,
   static cancel/space/idle event bits) and a player model get injected clocks, a fake I2S sink
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
    - Signal lifetime, rendezvous: a fake scheduler pauses a control between
      its `active_gen` lookup and its `SetBits`. The producer then completes
      and unregisters, and a second producer registers. On resume, the
      first producer's resources are already freed (an ASan host build
      detects any touch), the second producer sees a foreign `cancel_gen`
      and keeps playing, and the stale control reports its generation
      finished. A mutation that stores a producer handle in the controller
      and signals it must fail this test.
    - Gesture: with a fake player whose idle ack is delayed, (a) press during
      speech, release before the ack: no capture, no ask, verdict `stopped`;
      (b) press, ack, release 2 s later: capture length about 2 s counted from
      the ack, ask sent; (c) press, ack, release at 0.2 s: the 0.5 s floor
      applies from the ack; (d) a second press during the barrier: only the
      newest generation is consumed, and the older release is ignored;
      (e) ack later than 3 s after the press while still held: the arm is not
      expired. A mutation that reads a global released flag instead of the
      generation record must fail (a) and (d).
    - Roster: `speak` is in `s_caps[]` on all three board builds; on a
      `caps.speech=false` board every `speak` subcommand returns the typed
      refusal, never help text; the existing provision roster tests pass with
      the new entry.

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
   current at `[DONE]` (no replay of the old final card); glass takeover
   before `text_done` (a fake `render_ui`, `say` and `page` each injected
   after the first delta, and again between the last delta and `text_done`):
   later deltas still reach the relay summary, no append or commit reaches
   the glass, the `text_done` card and an old-backend `[DONE]` card are both
   freed unrendered, the summary gains ` [glass taken by <card_id>]` once,
   speech keeps playing, and the taking card is still current at `[DONE]`;
   the same three injected after `text_done` leave the stream untouched,
   since it has already ended; a cancel before `text_done` ends the ask's own
   stream with ` [stopped]` exactly once, and a cancel after takeover
   paints nothing; a mutation that drops the token check must fail;
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
