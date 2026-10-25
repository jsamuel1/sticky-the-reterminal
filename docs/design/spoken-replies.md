---
readtime: 18
description: "Design for spoken agent replies on the codec boards: speech streamed on the ask connection, 16 kHz PCM, no length cap, played as it arrives."
---

# Spoken replies — design (grammar v25, proposed)

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
  whole connection. Every ask path is also admitted work in the work/sleep
  gate (`tiny/tiny_admit.h`), so no sleep path powers down mid-ask.
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
that no longer exists. Every card-claiming path goes through that one choke
point: relay `render_ui`, `say`, `page`, the `messages` and `sensors` cards,
the home and page buttons, the auth cards and the sleep card. Two glass
writers bypass it today (a gallery flip and a `play` frame), and the lock
order below brings them under the same rule. With speech the ask lives
longer, so the rule becomes explicit:

- `tiny_display_stream_begin` returns a **stream token** (a nonzero `uint32_t`
  epoch) through an in/out argument. `stream_append`, `stream_commit`,
  `stream_note` and `stream_end` take that token. A voice ask's SSE begin
  passes the token from its listening frame back in and gets the same token
  out; the exact voice sequence is under "Ask-side token lifecycle" below.
- A takeover is any public glass write that replaces the card, stream on or
  not: `tiny_display_render_card` (every card type, image included),
  `tiny_display_gallery_nav` and `tiny_display_blit_1bit`. It claims a new
  glass generation (below), which retires any stream token. Later calls with
  that token return `ESP_ERR_INVALID_STATE` and touch
  nothing on the glass. Today only `render_card` ends the stream; a gallery
  flip or a `play` frame paints over it and the next commit paints the
  stream back, so these two join the rule. `tiny_display_flash_button` and
  `tiny_display_snapshot_index8` do not replace the card and are not
  takeovers. A rotation or scroll while a valid stream is on redraws the
  stream frame through the stream path and keeps the token, instead of
  today's reset.

**Glass generation.** Ownership of the glass is one monotonic
`uint32_t s_glass_gen`, guarded by `s_stream_mux` (below), never 0 (a wrap
skips 0). Every write that establishes a new picture **claims** the glass by
incrementing it: a stream begin (the stream token *is* the generation that
begin claimed), every takeover, and every redraw that is not a valid-token
stream redraw (`rerender`, `glance`, rotation and scroll with no stream on).
A stream op is valid only while `s_stream_on` and its token equals
`s_glass_gen`; continuing ops of a valid stream (append, commit, note, a
continuation begin, and rerender/glance/rotation/scroll redraws of that
stream) do not
claim. The rule is **latest claim wins**: a write paints only if its claim
is still the current generation at the moment it paints, checked in the
same critical section as the paint.

**Lock order.** A new non-recursive `s_stream_mux`, created statically in
`tiny_display_init` beside the other two (the lazy create in
`display_lock` goes), guards the stream state, the token and
`s_glass_gen`. The order is fixed and has one direction: **`s_stream_mux` →
`s_display_mux` → `s_region_mux`**. No path takes `s_stream_mux` while it
holds either of the others, and no path holds any of them across a network
fetch. The media store's `s_media_mux` ("Media store across a fetch"
below) sits after `s_stream_mux` and is never held with the display or
region locks.

To keep that order, today's re-entry goes. `stream_render` calls the public
`tiny_display_render_card` and `tiny_display_scroll` and tells them apart
with the `s_stream_rendering` flag. Each public entry point that paints is
split into a thin public **wrapper** and an internal **`*_impl`**. The
wrapper's whole job is: take `s_stream_mux`, call the impl, give
`s_stream_mux`. Every impl is **entered and exited with `s_stream_mux`
held**, takes `s_display_mux` itself as today, and never calls a public
display or shell entry point. The two impls that fetch (image and gallery)
are the only code that drops `s_stream_mux` while running, and only around
the fetch itself, as described under "A fetching takeover". `stream_render`
calls only impls; `s_stream_rendering` is deleted.

Wrapper / impl census (every painting entry point in `tiny_display.cpp`,
with today's internal re-entries):

| Public entry | Takes `s_stream_mux`? | Internal route after the split |
| --- | --- | --- |
| `render_card` | yes | `render_card_impl`. A `gallery` spec calls `gallery_commit_impl` → `gallery_nav_impl(0)` (today `render_gallery_card` → public `gallery_nav(0)`, a recursive acquisition after the split). An `image` spec calls `image_impl`. |
| `render_card` with `type:"universe"` | **no** | The wrapper parses the spec before locking (parsing needs no lock). For `universe` it calls `tiny_shell_universe_push()` holding nothing; that merges the roster and makes exactly one public `render_card` of a composite. Today this branch sits inside `render_card` and re-enters it from `render_universe()`. `render_card_impl` refuses a universe spec with `ESP_ERR_INVALID_ARG` rather than calling the shell. |
| `gallery_nav` | yes | `gallery_nav_impl(dir)` |
| `rerender`, `glance` | yes | valid stream: stream redraw, token kept (like rotation; today a clock-tick, lock or clean-flash rerender silently ends the stream); otherwise claim, `rerender_impl` → `render_card_impl(s_card_cache)` (a cached gallery reaches `gallery_nav_impl(0)` as above) |
| `set_rotation` | yes | valid stream: stream redraw; otherwise `rerender_impl`, and on `INVALID_STATE` the `rotated to N deg` card through `render_card_impl` |
| `scroll` | yes | valid stream: stream tail redraw; otherwise `scroll_impl` → `rerender_impl` |
| `scroll_page`, `scroll_gesture` | **no** | delegate to the public `scroll` (or, for a sideways swipe, to `tiny_shell_nav_back` / `tiny_shell_cycle`, which call public `render_card`) exactly once, holding nothing |
| `blit_1bit` (`play` frames) | yes | claim + paint in one critical section; no fetch (frames are prefetched) |
| `stream_*` | yes | token check, then `render_card_impl` / `scroll_impl` |
| `arm_wake_baseline` | no | calls public `render_card` between its own `display_lock` sections, holding nothing; it runs before any stream can exist |
| `flash_button`, `snapshot_index8`, `splash` | no | not glass owners: `s_display_mux` only, no card replaced, no claim |

A host census test fails if any impl, or any shell function reachable from
an impl, names a public painting entry point.

- **Stream ops** (`append` / `commit` / `note` / `end`) take
  `s_stream_mux`, compare the token with `s_glass_gen`, and if it is valid
  build the frame and paint it through `render_card_impl` (and `scroll_impl`
  for the tail) **before** releasing `s_stream_mux`. The token check and the
  paint are one critical section, so no takeover can land between them. A
  stream paint costs about 300–500 ms (a partial refresh), and that is the
  longest a takeover waits.
- **A non-fetching takeover** (every text, list, chat, menu, composite… card,
  and a `play` frame) takes `s_stream_mux`, claims a new generation (which
  retires any stream token), resets the stream, caches its spec, paints
  through its impl, and only then releases `s_stream_mux`. Nothing can land
  between its claim and its paint.
- **A fetching takeover** (an image card, a gallery open or flip, and a
  redraw of either) runs in three phases: (1) under `s_stream_mux`: claim
  `my = ++s_glass_gen`, reset the stream, cache the spec (`s_card_cache` is
  written here, so a concurrent rotation redraws *this* card, not the one
  it replaced), and build the claim's **immutable fetch record** from the
  media store ("Media store across a fetch" below); (2) release
  `s_stream_mux` and fetch from SD or the network into a buffer the claim
  owns, using only the record; (3) retake `s_stream_mux`, and paint through
  the impl only if `s_glass_gen == my`. If a newer claim landed during the
  fetch — a new stream begin, another takeover of either kind, a redraw —
  the fetched frame is **superseded**: it is not painted, it changes nothing
  in the media store (below), its buffer is freed, and the call returns
  `TINY_ERR_GLASS_SUPERSEDED` (a distinct code, mapped onto
  `ESP_ERR_NOT_FINISHED`). The image card's network fetch therefore never
  holds `s_stream_mux`, an old stream cannot repaint during it (its token
  died at the claim), and a late fetch cannot overwrite a newer owner. Two
  fetching takeovers resolve the same way regardless of which fetch
  finishes first: the earlier claim either paints and is then painted over,
  or finds itself superseded and drops; the final frame is always the later
  claim's. A gallery frame already in the PSRAM LRU is not a fetch: the
  claim paints it in phase 1, in the same critical section, and never drops
  `s_stream_mux`.
- **Media store across a fetch.** Paint generation fences only the glass.
  The gallery and photo state that a fetch reads or writes is a separate
  shared store. The invariant is strict and has no set-scoped exception:
  **a superseded claim leaves the store exactly as if it had never run**,
  apart from releasing what it reserved itself (its own buffer, its own
  target-page reservation). That holds whether the newer claim is a new set,
  the same set, a stream begin or a redraw. Every store mutation a fetch
  causes therefore happens in phase 3, after the paint, in the same
  `s_stream_mux` critical section as the generation check that let it
  paint. That store is:
  the gallery set (`s_gal_urls`, `s_gal_count`), the page indices
  (`s_gal_index`, the painted page, and `s_gal_target` with its owner
  `s_gal_target_gen`, the page the latest gallery claim asked for), a new
  set epoch `s_gal_set` (`uint32_t`, never
  0, bumped by every set commit), the two-slot PSRAM LRU, the SD files
  `/sdcard/gallery/NNN.raw` and `manifest.json`, the NVS manifest and page
  index, the photos archive `/sdcard/photos/NNNNN.raw`, its next sequence
  number `s_photo_seq`, and `s_photo_last_url`. The rules:
    - **Two guards.** A new non-recursive `s_media_mux` guards every SD
      gallery or photos file operation, `s_photo_seq`, `s_photo_last_url`
      and the NVS gallery writes. `s_stream_mux` guards the set, both
      indices, the LRU and the lazy NVS restore. `s_gal_set`, `s_gal_urls`
      and `s_gal_count` change only with **both** held, so either one is
      enough to read them. The order extends one way: `s_stream_mux` →
      `s_media_mux` → the SD driver lock and NVS (leaves). `s_media_mux` is
      never held together with `s_display_mux` or `s_region_mux`, and never
      across a network fetch.
    - **Set commit.** A gallery spec that is a new set (not a re-layout)
      commits in phase 1 with both locks held: write the URLs and count,
      bump `s_gal_set`, clear the LRU, reset the SD gallery directory and
      rewrite its manifest, save the NVS manifest, and set the target page.
      A spec is a re-layout only when the claim is a replay **and** its URLs
      equal the current set slot for slot; today's count-only test would
      let a replayed old spec of the same length keep a newer set's frames.
      The lazy NVS restore runs in the same place. A commit is bounded file
      work (at most 2 × `kGalMax` unlinks plus two manifest writes) and no
      network. A set commit is made only by a claim that is current at that
      moment, so it is not a superseded mutation.
    - **Immutable fetch record.** Phase 1 copies everything the fetch
      needs into a per-claim record: `{glass = my, set = s_gal_set,
      idx, count, url[256], replay}`. `idx` is computed from
      `s_gal_target` (not the painted page), and the claim sets
      `s_gal_target = idx` and `s_gal_target_gen = my`, so two quick flips
      land on two pages, not the same one twice. `url` is a copy, never a
      pointer into `s_gal_urls`; an image card's record likewise holds its
      own URL copy, so no pointer into `s_card_cache`, a caller's JSON or a
      cJSON tree that a newer claim can free crosses phase 2. `replay`
      replaces the `s_rerendering` global, which another task can flip
      while the lock is down. Phase 2 reads nothing from the store except
      under `s_media_mux`, as below.
    - **Buffer lifetime.** No pointer into the LRU leaves a critical section
      of `s_stream_mux`: an LRU hit is painted in phase 1, and the LRU frees
      a buffer (eviction, replacement, set commit) only under
      `s_stream_mux`. A buffer fetched in phase 2 belongs to its claim until
      phase 3 either hands it to the LRU (painted) or frees it (superseded).
      A claim frees only buffers it owns, so no claim can free a frame
      another claim is painting.
    - **Phase 2 is read-only.** The one SD operation phase 2 may do is a
      gallery frame read: take `s_media_mux`, proceed only if `record.set ==
      s_gal_set`, read `NNN.raw` into a claim-owned buffer, release. A file
      of the wrong size is reported as a miss and left in place (today's
      read path unlinks it); the painting claim's own frame write replaces
      it in phase 3, which already unlinks before its rename. A commit holds
      `s_media_mux` while it resets the directory, so no read sees a
      half-reset directory. A phase-2 read never holds `s_stream_mux`.
    - **Phase 3, superseded.** With `s_stream_mux` retaken and `s_glass_gen
      != my`: free the buffer; if `s_gal_target_gen == my` (no newer gallery
      claim has taken the target over) reset `s_gal_target` to
      `s_gal_index`. Nothing else: no LRU insert, eviction or age bump, no
      `NNN.raw` write, no index or NVS change, no archive.
    - **Phase 3, painting.** With `s_stream_mux` retaken and `s_glass_gen ==
      my`: paint (taking and releasing `s_display_mux` as today), then, still
      under `s_stream_mux` and with `s_display_mux` released, take
      `s_media_mux` and do the claim's store effects in this order: a
      network-fetched gallery frame is written to `NNN.raw` (tmp then
      rename); the page index is saved to NVS if it moved; an image card is
      archived (below). Release `s_media_mux`. Then the gallery frame
      enters the LRU keyed by `(set, idx)` and `s_gal_index` advances. The
      set is necessarily current here: a set commit is itself a claim, so
      a painting claim's set cannot have been replaced. The cost is that a
      stream op can wait one paint plus one frame write and one archive
      write (bounded file work, no network, no directory scan) behind a
      painting fetch claim.
    - **Photos archive, in paint order.** The archive is part of the
      painting claim's phase-3 section, so archive order is paint order by
      construction: a newer claim cannot paint, and so cannot archive,
      until the older one has archived and released `s_stream_mux`. Inside
      `s_media_mux`: skip if `record.replay` or the URL equals
      `s_photo_last_url`; otherwise write `/sdcard/photos/<s_photo_seq>.raw`
      (tmp then rename), and only after the rename succeeds advance
      `s_photo_seq` and set `s_photo_last_url` to the URL. `s_photo_seq` is
      found by one directory scan, the first time the archive runs after
      boot or after the card is remounted, not on every image (today's scan
      per image is unbounded in the archive's size). A failed write leaves
      both unchanged, so the next paint of the same URL tries again. A
      superseded picture that never reached the glass is not archived.
    - `tiny_display_gallery_count` takes `s_stream_mux` (it may run the lazy
      restore, which also takes `s_media_mux`) instead of reading the count
      bare.
- Callers treat `TINY_ERR_GLASS_SUPERSEDED` as "a newer picture already owns
  the glass", never as an empty or failed set: the empty-gallery fallback
  cards (`tiny_touch.cpp` `kGallery`, the relay `page gallery` branch) test
  for `ESP_ERR_INVALID_STATE` only and so never paint over the newer owner;
  the UP/DOWN gallery flip in `tiny_button.cpp` does not double-beep on it;
  a relay reply says `superseded`.
- **Rotation and scroll** take `s_stream_mux`. With a valid stream they
  redraw through the stream path while holding it and keep the token;
  otherwise they claim and call `rerender_impl` / `scroll_impl` (a cached
  image or gallery card redraws as a fetching takeover). Rotation has two
  callers since grammar 21: the `rotate` verb on `node_task` and the
  QMI8658 `orient` task (`tiny_orient.cpp`, AMOLED-1.8 `rotate auto`), which
  calls `tiny_display_set_rotation` from its own task. Both go through the
  one public `set_rotation` wrapper; its no-cache fallback card
  (`rotated to N deg`) is built through `render_card_impl` only when no
  stream is on. The `orient` task never takes `s_display_mux` directly, so
  an auto-rotation can wait at most one paint and cannot invert the lock
  order. It already skips rotation while locked.

The token, the generation, the takeover decision and the lock calls go into
a pure `tiny_stream_owner` with injected lock, fetch and paint callbacks, so
the host can test them; on the device the callbacks are the real mutexes,
the real fetch and the impls. The media-store rules go into a pure
`tiny_media_store` beside it, with injected SD, NVS, network and allocator
callbacks, for the same reason.

**Ask-side token lifecycle.** The stream API changes shape so no call can
run before a token exists:

```c
// *tok in: 0 = open a new stream; nonzero = continue the stream it names.
// *tok out: the stream's token, or 0 on any failure. `note` (nullable) is
// stored and painted in this call's frame.
esp_err_t tiny_display_stream_begin(const char *title, const char *card_id,
                                    const char *question, const char *note,
                                    uint32_t *tok);
// Owner-only card replacement: paints card_json iff tok is still current,
// as one critical section; the stream ends. Stale -> INVALID_STATE, nothing
// painted. Unlike stream_end, the card is not suppressed on a home stream.
esp_err_t tiny_display_stream_replace(uint32_t tok, const char *card_json);
```

A continuation (`*tok` nonzero) is accepted only for a valid token whose
stream is the wordless home stream and only with no `question`; it keeps the
frame (no second full wipe, exactly today's re-begin) and applies `note` by a
partial repaint if it changed. A stale `*tok` returns `ESP_ERR_INVALID_STATE`,
sets `*tok = 0` and paints nothing. A continuation of a valid token with a
question or a non-home card is a programming error: `ESP_ERR_INVALID_ARG`,
nothing changes. A fresh begin's failure (no stream buffer, render error)
resets the stream and returns `*tok = 0`.

The ask worker keeps the token in its per-request context (the same struct
that carries the request origin and the summary buffer), never in a global,
with a four-state **glass sink**: `unopened` (`tok = 0`, never owned),
`owned`, `detached` (owned once, lost to a newer claim) and `released`
(ended by this ask). The first stale return from any stream call moves
`owned` → `detached`. The context is zeroed at the start of every request,
so a replacement ask always begins with `tok = 0` and claims a new
generation. Every ask-owned glass write uses the token; the census of
today's call sites in `tiny_node.cpp`, all of which change:

| Today | New |
| --- | --- |
| voice: `stream_note("(( listening ))")` then `stream_begin("tiny","home",NULL)` (573–574) | `stream_begin("tiny","home",NULL,"(( listening ))",&tok)`: one full refresh, note already on it. Failure → `unopened`; capture proceeds and the answer may still open a fresh stream later |
| voice: `stream_note("(( thinking ))")` after capture (589) | `stream_note(tok, "(( thinking ))")` if `owned` |
| voice `voice_fail_card` (record / upload failure, via public `render_card`) | `owned`: `stream_replace(tok, card)`; `unopened`: public `render_card` (it is the newest claim); `detached`: not painted, summary only |
| SSE: `stream_begin(title,"home",question)` then `stream_note("(( thinking ))")` (429–430) | `stream_begin(title,"home",question,"(( thinking ))",&tok)`. Voice with `owned`: continuation, same token. Typed, or voice `unopened`: fresh begin. `detached`: no call |
| `ask_sse_line` append / commit (353–365), ` [stream cut]` append (461), `stream_end(final_card)` (467) | the same calls with `tok`, through the request context passed into `ask_sse_line`; skipped when not `owned` |
| classic JSON error card (778) and answer cards (788, 798) | `owned` (voice): `stream_replace(tok, card)`; `unopened`: public `render_card` as today; `detached`: not painted, the relay summary gets the text and ` [glass taken by <card_id>]` |

`text_done`, a barge-in cancel's ` [stopped]` end, and `stream_replace` move
the sink to `released` and zero `tok`. No other firmware file calls the
stream API (`tiny_display_stream_active()` has no callers and keeps its
meaning).
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
- **Player wake.** The player's prebuffer wait (start, and after an
  underrun's amp-off) follows the same check-before-wait rule as the
  producer. Its predicate, read under the ring lock, is "fill ≥ 300 ms, or a
  terminal state is published". The producer calls `xTaskNotifyGive(player)`
  after every committed chunk and after publishing a terminal state; the
  player handle is permanent (created at boot, never deleted), so this does
  not break the signal-lifetime rule. The player checks the predicate, then
  calls `ulTaskNotifyTake(pdTRUE, …)`. A task notification is a counter, not
  an edge: a give between the check and the take leaves the count nonzero,
  so the take returns at once and the predicate is checked again. No commit or
  terminal publish can strand a playable ring.
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

### Status light (FNK0104S WS2812)

The status light (`tiny_led`, `CONFIG_TINY_STATUS_LED`) today shows blue
while capturing, amber while an ask is in flight, and red for 5 s after an
askfail card. Its ask field is lowered to `IDLE` by `LedGuard` when the ask
returns. With speech, the ask worker stays alive through the audio tail, so
without a change the light would sit amber for the whole spoken answer and
say "thinking" while the device is talking. Speech gets its own input with
one owner:

- `tiny_led_inputs_t` gains `bool speaking`, and `tiny_led.h` gains
  `tiny_led_speech(bool)`. **The player is the only writer.** It sets
  `speaking` when it turns the amp on for a speech generation, and clears it
  in the same step as its idle acknowledgement, after amp-off. Chimes and
  `play` frames never set it. The S1 bench harness and, from grammar 25,
  `speak test` do, because they go through the same player path.
- The ask lowers its own field from `THINKING` to `IDLE` at the first
  accepted `text_done`: the text is finished, so it is no longer thinking.
  An ask that never sees `text_done` (old backend, or a cut before it)
  keeps today's `LedGuard` rule. `LedGuard` still runs at the epilogue and
  is idempotent.
- Colour: `TINY_LED_RGB_SPEAKING` `{0, 200, 60}` (green), new in
  `tiny_led_state.h`. Precedence, strongest first: locked or asleep → off;
  capturing → blue; ask `THINKING` → amber; speaking → green; error → red;
  otherwise off. So audio that arrives before `text_done` shows amber (the
  model is still writing), and the audio tail after it shows green.
- Barge-in: `speaking` clears at the idle acknowledgement, and capture's
  `tiny_led_capture_begin` comes after that ack, so the light goes green →
  blue with no amber between and never shows blue while the amp is on. A
  hold-to-talk released before the ack (stop only) goes green → off.
- `audio_error`, `cut` and `refused` are speech verdicts, not ask failures:
  they do not call `tiny_led_error()`, which stays reserved for the askfail
  card as today.
- Pocket lock: speech continues, but the light is dark, the same
  precedence as today (§11: a pocket shows nothing). Deep sleep cannot be
  claimed while speech holds its admission count, and OTA crosses the speech
  barrier first, so `speaking` is already clear before
  `tiny_led_prepare_sleep()`. Shutdown's forced amp-off path also clears
  it.
- Other boards: `tiny_led_speech` is an inline no-op like the rest of the
  API, so callers need no `#if`.

Host coverage extends `tests/host/led_host_test.sh`: the resolve table with
`speaking` crossed against every existing input; `THINKING` + `speaking` is
amber and `speaking` alone is green; a sequence model of a spoken voice ask
(PENDING → blue → amber → amber with audio before `text_done` → green after
it → off at idle ack); a barge-in sequence with no black or amber frame
between green and blue; `audio_error` never giving red; and locked dark
while `speaking`. A mutation that makes the ask worker, not the player,
clear `speaking` must fail the barge-in sequence (it would turn green off
before amp-off).

## Cancellation and task ownership

Barge-in is a request to owners, never cross-task resource destruction.

**Signal lifetime.** No control path ever holds a pointer, task handle or
callback that belongs to a producer. The speech controller owns one static
event group, created at boot and never deleted, plus three generation words
(`active_gen`, `cancel_gen`, `ack_gen`) guarded by a `portMUX_TYPE`.

**Creation order.** The group is built with `xEventGroupCreateStatic` from
a file-scope `StaticEventGroup_t`, in `tiny_speech_init()`. The player task
is **not** created by `init_inputs`: on the codec boards `app_main` calls
`sticky_buzzer_init()` right after `tiny_display_init()`, and
`sticky_buzzer_init()` → `audio_es8311_init()` creates `audio_play` at
priority 5, above the boot task, so the scheduler may run the player before
`xTaskCreateWithCaps` even returns. `tiny_audio_init()` in `init_inputs`
later calls `audio_es8311_init()` again and gets the memoised result; it
creates nothing. So `app_main` calls `tiny_speech_init()` directly after
`tiny_display_init()` and **before** `sticky_buzzer_init()`, on every board
and on both the cold-boot and glance-wake paths (the buzzer init runs before
the `glance_wake` split). The boot order is:

`board_init()` → `tiny_display_init()` → **`tiny_speech_init()`** →
`sticky_buzzer_init()` (codec boards: `audio_es8311_init()` creates the
player and records its permanent handle) → … → `init_inputs`
(`tiny_audio_init()` is a memoised no-op for the player, then
`tiny_led_init()`, `tiny_button_init()`) → `tiny_node_start()` and the ask
worker.

`tiny_speech_init()` has no board `#if` and depends on nothing: a static
event group, a statically initialised `portMUX_TYPE` and zeroed generation
words. `xEventGroupCreateStatic` with a non-null buffer cannot fail, so the
call returns `void` and cannot leave a half-built controller. On the
reTerminal (buzzer, `caps.speech` false) the group exists and nothing ever
waits on it; every control entry still answers with the typed
`speak refused: no speech playback on this board` before it touches the
group. The player handle the producer notifies (see Player wake) is written
by `audio_es8311_init()` before it returns; producers exist only after
`tiny_node_start()`, so no producer can read it unset. The status-light
calls the player makes (`tiny_led_speech`) already tolerate a light whose
`tiny_led_init()` has not run (`s_mux` null → no-op), and a speech start
cannot reach the player before the node exists.

On a glance wake `init_inputs` runs after the shell paint; until it runs
nothing can produce or cancel speech, since the buttons and the node do not
exist yet, but the group already does. Every control and producer entry
point, and the player's speech path (it sets `SPACE_BIT` and `IDLE_BIT`),
still checks that the group handle is non-null: a control or producer
returns `speech not ready`, and the player refuses a speech-start command
and keeps playing tones only. A future reordering therefore fails closed
rather than dereferencing a null handle. There is no lazy create: the display's lazy
`xSemaphoreCreateMutex` pattern would race two first callers, and this
group must have exactly one instance. The bits are fixed at compile time
(`CANCEL_BIT` 0, `SPACE_BIT` 1, `IDLE_BIT` 2); nothing allocates them at
runtime, and no other module shares the group.

**Clear before check.** Each waiter clears the bit it is about to wait for,
then re-reads both its durable predicate and the relevant generation word before
it calls `xEventGroupWaitBits(..., xClearOnExit = pdFALSE, ...)`. For
`CANCEL_BIT` and `IDLE_BIT`, the durable predicates are `cancel_gen == G` and
`ack_gen == G`. For `SPACE_BIT`, the producer locks the ring and rechecks that
free space is still smaller than the complete decoded chunk it must commit.
A notification that lands between clear and predicate check is therefore seen
by the predicate; one that lands after the check leaves its bit set for the
wait. Setting a bit nobody is waiting for is harmless because every wake is
confirmed against its generation or ring predicate.

- A producer registers in this order: clear `CANCEL_BIT` and `SPACE_BIT`
  (outside any lock), then set `active_gen` under the lock. A control that
  runs before the publish sees the old `active_gen` (zero, or a finished
  generation) and cannot target the new producer. A control that runs
  after it sets `cancel_gen` and then the bit, and the clear has already
  happened, so registration can never erase a real cancel. It unregisters by
  setting `active_gen = 0` under the lock. Registration never stores a task
  handle or a sink. **No FreeRTOS call is made inside the `portMUX`**:
  ESP-IDF forbids blocking or event-group calls in a spinlock critical
  section, so the lock covers only reads and writes of the three words.
- A control reads `active_gen` under the lock. If it is zero the control
  answers `not speaking`. Otherwise it sets `cancel_gen = active_gen`, leaves
  the lock, and calls `xEventGroupSetBits(CANCEL_BIT)` on the static group.
  Every caller (the button callback in the esp_timer task, `node_task`, the
  OTA and shutdown paths) is a task, never an ISR.
- The producer's full-ring loop clears `SPACE_BIT | CANCEL_BIT`, checks
  `cancel_gen` against its generation, then checks free ring space under the
  ring lock against the whole pending chunk before it waits on
  `SPACE_BIT | CANCEL_BIT`. A player free between clear and the capacity check
  is observed as space; a free after the check leaves `SPACE_BIT` set. On wake
  it repeats the complete clear → cancellation/capacity checks → wait protocol.
  A foreign cancellation generation is ignored; neither a stale bit nor a
  lost space edge can strand a writable ring.
- The player acknowledges idle the same way: it writes `ack_gen` under the
  lock, leaves it, then sets `IDLE_BIT`. Waiters compare `ack_gen` with the
  generation they stopped.

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
   replacement slot is retained. Relay, typed and offline requests arriving
   after that slot is occupied receive an explicit busy reply. BOOT is
   different because its release event must stay paired with its press: a newer
   BOOT long-press atomically replaces an older pending physical slot and its
   gesture generation before signalling the worker. The superseded generation
   can neither capture nor send an ask; its later release is ignored by the
   generation check. A BOOT press that finds the slot held by a relay, typed
   or offline request is refused: it writes no gesture record, so its release
   finds no matching generation and does nothing, and the status line reads
   `busy: replacement pending`. Speech still stops, because the pending
   request has already signalled cancellation. The reTerminal fixed-length
   click carries no gesture and follows the non-BOOT busy rule. Before the first audio sample, replacement asks retain
   today's behavior and are refused as `ask already in flight` rather than
   cancelling an agent turn that has not begun speaking. Completion is posted
   back to the node task so
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
   button signals locally. `miccheck` and `speak stop` signal then wait
   on the same bounded idle barrier. A barrier timeout returns `speech stop
   timed out` and does not capture with the amp active. Repeated stop
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
  `{G, pressed_at, released = false}` under the gesture `portMUX`. With no
  pending physical replacement it claims that fixed slot. If an older BOOT
  replacement is still pending, the same critical section replaces both slot
  generation and gesture record with the new `G`; there is never an old queued
  request pointing at a new gesture. A newer non-BOOT request does not mutate
  this record and follows the busy rule above. `on_talk_release` sets
  `released` and `released_at` only when the record and pending/capturing slot
  still hold the generation that was pressed. Every path uses this record,
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

### Work/sleep admission

Main's `tiny_admit` gate (`tiny/tiny_admit.h`, grammar 23) is the one door
between work and deep sleep: every ask path enters it in
`tiny_node_do_ask`, and the `sleep` verb (`work_to_sleep`), the PWR key
(`tiny_node_sleep_now`) and auto-sleep (`sleep_claim`) refuse while any work
is counted. Speech moves the ask off `node_task` and outlives the text, so the
gate's count has to follow it:

- A request queued to the ask worker is reserved work: the producer (relay
  dispatch, button callback, offline drain) calls `work_enter()` before it
  publishes into the request queue or the replacement slot, and calls
  `work_exit()` itself if the publish fails or a newer BOOT press replaces
  the slot. The worker adopts the count with `Work(AdoptReserved::Yes)`,
  the shape `tiny_button.cpp` and `tiny_touch.cpp` already use. Sleep cannot
  claim between queue acceptance and worker start.
- That one count lives until the request's **speech generation is idle**:
  the worker holds it across `[DONE]`, the residual drain and amp-off, and
  releases it only after the player's idle acknowledgement for that
  generation (or at once when no speech was opened). The text answer
  finishing does not end the work, because the amp is still on.
- `speak test` enters the gate in its verb before it starts the test
  producer and hands the count to that task by the same adopt rule; the
  task releases it after its own idle acknowledgement. It answers
  `refused: going to sleep` when the gate is claimed, as every verb does.
- The player, the static event group and the queues are boot objects and
  take no count. A chime enqueued during a sleep claim is unaffected (the
  sleep path already plays none after the claim).
- No speech path ever calls `sleep_claim()`, `work_to_sleep()` or
  `sleep_release()`, and no sleep path waits on the speech barrier: a sleep
  request during speech is refused, never queued behind it. This keeps one
  owner for the power transition and leaves the gate's existing
  `sleep refused: busy` answer correct for spoken answers too.

Host tests reuse `tests/host/admit_host_test.sh`'s real-thread harness with
the fake player: (a) a sleep claim raced against a relay `ask` publish at
every point between `work_enter()` and the worker's adopt never sleeps with a
request queued; (b) `sleep_claim()` fails from `[DONE]` until the idle ack of
that generation, and succeeds immediately after; (c) a BOOT replacement that
supersedes a pending slot leaves the count unchanged (one exit, one enter);
(d) `speak test` under a held claim answers `refused: going to sleep` and
starts no task. Mutations that release at `[DONE]`, release before the idle
ack, or let the worker construct a fresh `Work` instead of adopting must fail
(b) or (a).

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
| pocket lock | speech continues. Lock guards inputs, not output (same as `say`); the status light is dark |
| `sleep` verb, PWR-key long press or auto-sleep | speech is admitted work (see "Work/sleep admission" below), so every sleep path refuses exactly as it does for an ask in flight today: `sleep refused: busy (...)`, the PWR key does nothing, auto-sleep is due again next tick. Sleep never cancels speech; `speak stop` then `sleep` does |
| OTA reboot or node shutdown | signal cancellation, wait for the bounded player-idle/amp-off barrier, then cross the power boundary; on timeout refuse the OTA, while shutdown's last resort forces the amp GPIO off before teardown |
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
filter on the verb roster. Since grammar 22 the roster has two parts in
`tiny_node.cpp`: `s_caps[]`, the static list every board shares (the
provisioning tools, `provision_lib.py` and `provision.js`, parse it from
source), and `s_board_verbs[]`, verbs appended only where a board-table bit
is set (today only `brightness`, on `caps.brightness`). The heartbeat,
`pair/start` and the BLE tool catalog all read the combined list. `speak`
goes in `s_caps[]`, not `s_board_verbs[]`: whether speech works is not a
fixed board fact (AMOLED-1.8 stays false until its ES8311 is proven, and
`speech_ready` is a runtime result), so a board-gated entry would either
advertise a verb that refuses or hide one that a later build enables. This
design adds no TinyBeacon capability bit.

## Grammar v25 (proposed)

The main branch is already at grammar 24: v19 is strict direct-artifact OTA
arguments; v20 adds `sensors` `battery.source` and FNK0104S `battery_pct`;
v21 adds AMOLED-1.8 QMI8658 orientation/acceleration, `rotate auto`, and the
related capability/status shapes; v22 adds the colour-board `brightness`
verb (board-gated through `s_board_verbs[]`), `caps.brightness` and the
`status` `brightness` field; v23 adds AMOLED-1.8 AXP2101 power and the
PCF85063A clock (`usb_power`, `vbus_mv`, `battery.source` `axp2101`) and the
`sleep refused: busy` / `refused: going to sleep` answers of the work/sleep
admission gate (`tiny/tiny_admit.h`); v24 adds the internal-RAM receipts
`status` `internal_largest` and `internal_boot` (`tiny_heapmark`). Spoken
replies are the next grammar change after v25 (manual rotation, which
took the number this design first proposed). If another grammar change lands
first, this proposal takes the next free
number; the number is assigned when the complete public firmware slice is cut,
not by this document.

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
count moves from 23 shared verbs (24 on the colour boards, with
`brightness`) to 24 shared verbs (25 on the colour boards).

**Status.** `caps.speech` (hardware bool), `speech_ready` (player/ring bool),
`speaking` (bool), `speak_last` (the `speak status` verdict), and `speakmode`.

**Settings.** One row on the settings page, persisted in NVS
(`tinycfg/speakmode`): `off | voice asks | all asks`, default `voice asks`.
This is a closed enum and an owner decision (2026-10-24): `all asks` is
public, so typed and offline-typed asks may opt into speech. An unknown
stored value reads as the default.

CHANGELOG and `TINY_GRAMMAR_VERSION` move to 25 in the same atomic public
change that adds the request/events, `speak` verb, `caps.speech`, status reply
fields and setting. The slices below are implementation order, not permission
to expose a partial grammar. S1 keeps new helpers unreachable from existing
verbs/status and uses host tests plus a compile-time bench harness; it may land
alone only in that wire-inert state. Otherwise S1 and S2 are squashed into one
buildable commit. No build—tagged or not—may dispatch or advertise any new
speech surface while reporting grammar 24.

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
| Sleep | the ask is admitted work (`tiny_admit`); `sleep`, the PWR key and auto-sleep refuse until it returns | the same count is held until the speaker is idle, so sleep refuses for the whole audio tail rather than cutting it |
| Cancellation | none today; the turn runs to `[DONE]` | the device closes the connection; the backend must stop TTS (contract item 6). Tool side effects already done are not rolled back |
| Glass | typed live, then the final card | unchanged up to `text_done`; audio never touches the glass |
| Status light (FNK0104S) | amber while the ask runs | amber until `text_done`, green while the speaker plays |
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

1. **S1, wire-inert firmware core, no backend dependency.** `tiny_pcm`
   gets the canonical base64 validator/decoder, the mono→stereo gain expander
   and the ring. A pure-C speech controller (generations, terminal state,
   static cancel/space/idle event bits) and a player model get injected clocks,
   a fake I2S sink and a fake amp pin so they run on the host.
   `audio_es8311` gets the ring player with prebuffer, underrun, residual drain,
   flush and the amp-off acknowledgement. The status-light resolver may gain
   an internal `speaking` input because that changes no wire shape. S1 does
   **not** add `speak` dispatch, `s_caps[]`, `caps.speech`, speech status fields,
   `speakmode`, ask opt-in or SSE parsing while grammar remains 24. Its hardware
   proof uses a compile-time bench harness that invokes the player directly;
   the harness is off in normal builds. If implementation needs the public
   `speak test` vehicle instead, S1 and S2 land atomically with grammar 25.
   S1 host tests, each pinning one contract:
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
    - Event bits: a fake scheduler injects a matching generation or ring-space
      transition at every point between a waiter's clear, its durable-predicate
      recheck and its `WaitBits`; each transition wakes or is seen by the
      recheck, and none is lost. For `SPACE_BIT`, free fewer than, exactly and
      more than the pending complete chunk, including a player free immediately
      before clear, between clear and ring-capacity check, and between that
      check and wait. Player wake: a commit or terminal publish injected at
      every point between the player's prebuffer check and its
      `ulTaskNotifyTake`, including a sub-prebuffer `done`, is never lost. A
      mutation that replaces the counting notification with a cleared edge
      flag and no recheck must fail. Registration rendezvous: a control lands (a) between the
      producer's bit clear and its `active_gen` publish, where it answers
      `not speaking` or targets the old generation, and the new producer plays
      on; and (b) immediately after the publish, where the new producer's first
      wait or re-read sees the cancel. A fake `portMUX` asserts that no FreeRTOS
      fake is called while it is held. A control or producer entry before
      `tiny_speech_init()` returns `speech not ready` and touches no handle.
      Startup order: the boot sequence is factored into one
      `tiny_boot_sequence(const tiny_boot_ops_t *)` table that `app_main`
      itself calls (cold and glance-wake arms), so the host test runs the
      **real** `app_main` ordering rather than a copy of `init_inputs`. Its
      ops are fakes: `sticky_buzzer_init` runs a fake `audio_es8311_init`
      whose fake `xTaskCreateWithCaps` runs the new player task immediately,
      before its creator resumes. On both arms the player's first
      instructions find a non-null group, and no control, producer or player
      call ever sees a null handle; the later `tiny_audio_init()` creates no
      second player. With the group deliberately absent, the player refuses
      speech-start and still plays tones. Mutations that move
      `tiny_speech_init()` after `sticky_buzzer_init()`, or back into
      `init_inputs`, must fail the immediate-run case; a mutation that makes
      `tiny_audio_init()` create the player instead must also fail it on the
      glance-wake arm, where `init_inputs` runs after the paint.
      Mutations that clear after publish, omit the ring-capacity recheck, or
      clear after a predicate check must fail the corresponding rendezvous.
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
      applies from the ack; (d) a second BOOT press during the barrier
      atomically replaces both the pending physical slot and gesture record;
      only its generation is consumed, while the older release is ignored and
      no orphaned slot remains; (d2) a BOOT press while a relay or typed
      request holds the slot is refused without writing a gesture record,
      its release changes nothing, and the pending request is the one
      consumed; (e) ack later than 3 s after the press while
      still held: the arm is not expired. A mutation that updates only the
      gesture record but leaves the pending request's old `G`, or reads a global
      released flag, must fail (a) and (d).
    - Public roster/grammar (S2 or the atomic S1+S2 commit): `speak` is in
      `s_caps[]` (not `s_board_verbs[]`) on all three board builds, so the
      combined roster is 24 verbs on the reTerminal and 25 on FNK0104S and
      AMOLED-1.8; on a `caps.speech=false` board every `speak` subcommand
      returns the typed refusal, never help text; the existing provision
      roster tests pass with the new entry; every build exposing the entry
      reports grammar 25.

   Proof on the FNK0104S bench uses the direct compile-time S1 harness, or the
   `speak test` verb only after the atomic grammar-25 public slice. The latter
   replies `test started`; its verdict is read with `speak status`:
    - `speak test 60` reaches `done 60.0 s, 0 underruns`, and `miccheck`
      reads the room floor afterwards (the amp is really off).
    - A 1 s test, which is longer than the prebuffer but proves nothing about
      the tail, and a test whose producer is paused halfway to force an
      underrun followed by a sub-prebuffer tail, both end `done` with the amp
      off.
    - The AI button pressed mid-test stops it, and capture starts only after
      the log's amp-off acknowledgement.
    - Grammar-22 slice only, since S1 has no relay `speak` surface: relay
      `speak stop` sent twice gives the verdict, then `not speaking`. Under
      the S1 harness the same idempotence is proven by calling the controller's
      stop entry twice from the harness.
    - A chiming `render_ui` mid-test is dropped and counted (`render_ui` is an
      existing verb, so this runs under either vehicle).
    - The status light shows green for the whole test and goes dark at the
      amp-off acknowledgement; an AI-button barge-in goes green → blue.
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
   since it has already ended; a gallery flip and a `play` frame before
   `text_done` retire the token like a card; manual and QMI8658-driven
   auto-rotation plus scroll with a valid stream keep it and redraw the stream
   under the same stream-first lock order; a cancel before `text_done` ends the
   ask's own
   stream with ` [stopped]` exactly once, and a cancel after takeover
   paints nothing; a mutation that drops the token check must fail;
   `tiny_stream_owner` lock order, with fake recursive-detecting mutexes
   and a fake paint that can pause: (i) a commit validates its token and
   pauses inside its paint, a takeover arrives, the commit's paint
   completes, then the takeover retires the token and paints, and the
   takeover's card is the last frame; no later stream paint follows;
   (ii) a takeover claims and pauses inside its (fake image) fetch, and a
   commit of the old stream arriving then returns stale without painting;
   (iii) a stream paint that would re-enter a public display entry point
   trips the fake mutex's non-recursive assert, and the same assert runs
   over every census route: a cached-gallery rerender, a gallery spec
   through `render_card`, a `universe` spec through `render_card` (shell
   push → one composite render), `scroll_page` and a sideways
   `scroll_gesture`, each with and without a valid stream; (iv) a
   lock-order checker records every acquisition and fails on any
   `display` → `stream` or `region` → `display` edge; (v) a takeover pauses
   in its fetch, a new `stream_begin` claims and paints its frame, then the
   fetch completes: the takeover returns `superseded`, paints nothing, does
   not advance the gallery index or its NVS save, and the new stream's later
   commits still paint; (vi) takeover A pauses in its fetch, takeover B
   (fetching, then again non-fetching) claims and paints, then A completes:
   A is superseded and B's frame is last; and with B's fetch slower than
   A's, A paints, B paints over it, and B is last; (vii) a rotation and a
   rerender with no stream on, landing during a takeover's fetch, redraw
   the takeover's own cached spec and supersede its late paint, never
   resurrecting the card it replaced; (viii) the `kGallery` touch route and
   relay `page gallery` given `superseded` paint no fallback card. Mutations
   that release `s_stream_mux` between the token check and the paint, hold
   it across the takeover's fetch, or drop the post-fetch generation check
   must fail (i), (ii) and (v)/(vi) respectively; a static census check
   fails if any `*_impl` or impl-reachable shell function names a public
   painting entry point.
   `tiny_media_store`, with a fake SD (a map of path → bytes, plus
   pausable read and write), a fake NVS, a pausable fake network and a
   tracking allocator that fails on a double free, a free of a buffer it
   did not hand out, or a read of a freed buffer. Gallery A (set `a0..a3`)
   and gallery B (set `b0..b3`, same length, different bytes) throughout:
   (ix) network path: an A flip pauses in its network fetch of `a2`, B
   commits and paints `b0`, then the fetch completes: A returns
   `superseded`, `/sdcard/gallery/002.raw` is absent, the manifest and the NVS manifest name B's URLs, the NVS page index is
   B's, the LRU holds only B-set entries, and a later B flip to page 2
   fetches B's URL, not A's; (x) SD path: the same with A paused inside its
   SD read of `002.raw`: B's commit waits for that read to finish, then
   resets the directory; A's buffer is freed by A, and nothing of A is in
   the LRU; and the reverse order, B's commit holding the media lock while
   A's read arrives, makes A's read miss (set changed) without touching a
   file; (xi) LRU path: A's page is an LRU hit, so A paints in phase 1 and
   B's commit, arriving during that paint, waits on `s_stream_mux`, then
   frees the LRU entries; the tracking allocator sees no read of a freed
   buffer; (xii) a fetch
   paused across two commits (A → B → A again, same URLs as the first A)
   is still superseded, since the set epoch, not the URL list, names the
   set; (xiii) two quick flips on one set, the first paused in its fetch,
   land on pages +1 and +2, and the superseded first flip neither advances
   `s_gal_index` nor its NVS save; (xiv) a replay of an old cached gallery
   spec of the same length as the current set commits a new set rather than
   keeping the current set's frames; (xv) two image cards whose fetches
   finish together archive under two distinct sequence numbers, a
   superseded image is not archived, and an image claim whose source spec
   is freed by a newer `render_card` during its fetch still fetches,
   archives and frees only its own URL copy and buffer; (xvi) a
   lock-order checker over all of
   the above fails on any `media` → `stream`, `media` + `display` or
   `media` held across a fake network call; (xvii) **same-set
   supersession**, one set A with the LRU preloaded as `{a0 (older),
   a1 (newer)}` and `000.raw`/`001.raw` on the fake SD: a flip to page 2
   pauses in its network fetch of `a2` (and, as a second arm, a flip to
   page 3 pauses in its SD read of `003.raw` placed there first), then a
   newer claim lands on the same set, once for each kind: a new
   `stream_begin`; a rerender of the cached gallery spec (same set, LRU
   hit, so no fetch); another gallery claim to page 1 (LRU hit) and, as a
   further arm, to page 3 by network. When the paused fetch completes it
   returns `superseded` and the test asserts the exact store against a
   snapshot taken before it started, minus only what the newer claim did:
   the fake SD's files and bytes (`002.raw` never written; `003.raw`
   neither written nor unlinked), the LRU's entries, keys and age order
   (no insert, no eviction of `a0`, no age bump), `s_gal_index`, the NVS
   page index and its write count, and that the target page is the newer
   gallery claim's, or back at `s_gal_index` when the newer claim was a
   stream or rerender; the tracking allocator shows the superseded buffer
   freed exactly once by its own claim; (xviii) **archive order**: image
   A paints and pauses inside its archive write (fake SD write paused,
   `s_stream_mux` held); image B's fetch completes and blocks retaking
   `s_stream_mux`; A resumes and finishes; B paints and archives; then B
   is sent again. The archive holds exactly `00001.raw` = A's bytes and
   `00002.raw` = B's bytes, `s_photo_last_url` is B's URL, the B resend
   writes nothing, and a later resend of A archives `00003.raw`; a fake
   SD write failure for A leaves `s_photo_seq` and `s_photo_last_url`
   unchanged. Mutations that read
   `s_gal_urls` after phase 1, drop the set check before an SD read, let a
   superseded claim save the page index, free an LRU entry outside
   `s_stream_mux`, let a superseded claim write `NNN.raw` or insert into
   the LRU when its set is still current, or run the archive after
   `s_stream_mux` is released must fail (ix), (x), (xiii), (xi), (xvii)
   and (xviii) respectively.
   Ask-side tokens, with fake display seams: (a) a voice ask runs
   listening begin → `(( thinking ))` note → SSE continuation begin →
   deltas → `text_done`, and every call carries one token that the
   continuation returned unchanged; there is exactly one full refresh before
   the first delta; (b) a takeover after the listening frame (during capture
   and again during upload) makes the thinking note, the continuation begin
   and every delta return stale with nothing painted, the sink reads
   `detached`, and a record or upload failure card is not painted over the
   newer card; (c) a listening begin that fails (fake allocation failure)
   leaves `tok = 0`, the capture still runs, and the SSE path opens a fresh
   stream; (d) a typed ask always opens fresh, and a continuation with a
   question or a stale token is refused without painting; (e) the classic
   JSON answer of a voice ask with a still-valid token replaces the
   listening frame through `stream_replace`, and with a dead token paints
   nothing; (f) two consecutive asks never share a token. A mutation that
   restores the note-before-begin order must fail (a), and one that lets a
   fresh begin reuse a stale token must fail (b);
   button and relay replacement asks; a second BOOT press while the first
   replacement waits for amp-off atomically coalescing pending slot + gesture
   generation; later release of the superseded press doing nothing; no double
   reply, orphaned replacement or capture before amp-off. Sleep tests
   (`sleep` verb, PWR key, auto-sleep) pin that a queued request or a
   speech tail holds the admission count and every sleep path refuses
   without cancelling it ("Work/sleep admission" (a)-(d)); OTA tests pin
   cancellation-before-power transition
   and timeout refusal. Shutdown tests pin amp-off even when normal
   acknowledgement fails. Mutation controls that remove event reset, atomic
   ring commit, terminal-tail wakeup, generation checks, the speech-tail
   admission count
   or the OTA power-boundary barrier must fail. Proof on hardware: a voice ask longer
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
  ask plus one short relay/heartbeat client, then add explicit margin. Main's
  v24 `status` `internal_largest` and `internal_boot` receipts are the
  measurement source: read them during a voice ask that overlaps a relay
  poll. Until
  that floor is measured on FNK0104S, `speech_ready` stays false in release
  builds; do not ship a guessed threshold that can turn allocator pressure into
  a reboot.
- Ring size: 1 MB (about 32 s of text lead) or more. PSRAM is 8 MB, but the
  canvas, frame prefetch and the ask buffers already live there; measure the
  free PSRAM during a `play` before raising it.
