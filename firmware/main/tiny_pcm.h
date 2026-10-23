// tiny_pcm — board-independent PCM helpers for the codec boards.
//
// Pure C++ (no ESP-IDF headers) so tests/host can compile the shipped file:
//   * the 44-byte WAV header the voice upload carries;
//   * the chime vocabulary as note lists, plus an RTTTL parser, so
//     tiny_audio_chime("ack") and tiny_audio_chime("tune:d=8,o=6,b=180:c,e,g")
//     both turn into the same {hz, ms} sequence;
//   * a sine tone renderer (interleaved stereo s16, raised edges so a note
//     never starts or stops on a click);
//   * RMS / peak statistics for capture logs and miccheck.
#pragma once

#include <stddef.h>
#include <stdint.h>

struct TinyNote {
    uint16_t hz;  // 0 = rest
    uint16_t ms;
};

// Canonical PCM WAV header for mono 16-bit samples at `sample_rate`.
void tiny_pcm_wav_header(uint8_t h[44], uint32_t sample_rate, uint32_t pcm_bytes);

// Named chime -> notes. Returns the count written, 0 for an unknown name.
// Names: "beep"/"ok" (accept), "double"/"refuse" (dead end), "ask", "ack",
// "error". The beep and double timings match the reTerminal buzzer.
size_t tiny_pcm_chime_notes(const char *name, TinyNote *out, size_t cap);

// RTTTL ("name:d=4,o=5,b=100:8c6,p,8e6.") -> notes. Returns the count
// written (truncated at `cap`), 0 when the string is not RTTTL. Notes above
// `max_hz` (the renderer's Nyquist margin) become rests.
size_t tiny_pcm_rtttl(const char *s, TinyNote *out, size_t cap, uint16_t max_hz);

// One note being rendered. Init with tiny_tone_start, then call
// tiny_tone_render until it returns 0.
struct TinyTone {
    uint32_t rate;
    uint32_t total;   // frames in the note
    uint32_t pos;     // frames already rendered
    uint32_t phase;   // DDS accumulator
    uint32_t step;    // DDS increment per frame
    uint32_t fade;    // frames of rise/fall at each edge
    int16_t amp;      // peak amplitude
};

void tiny_tone_start(TinyTone *t, uint32_t rate, const TinyNote &n, int16_t amp);
// Writes up to `frames` stereo frames (L = R) into `stereo`; returns frames
// written. A rest writes zeros.
size_t tiny_tone_render(TinyTone *t, int16_t *stereo, size_t frames);

struct TinyPcmStats {
    int rms;
    int peak;
    int nonzero_pct;
};
// Statistics over `n` samples taken every `stride` samples from `s`.
TinyPcmStats tiny_pcm_stats(const int16_t *s, size_t n, size_t stride);
