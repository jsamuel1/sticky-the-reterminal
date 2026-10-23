#include "tiny_pcm.h"

#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void tiny_pcm_wav_header(uint8_t h[44], uint32_t sample_rate, uint32_t pcm_bytes)
{
    const uint32_t byte_rate = sample_rate * 2;  // mono s16
    memcpy(h, "RIFF", 4);
    uint32_t v = 36 + pcm_bytes;  memcpy(h + 4, &v, 4);
    memcpy(h + 8, "WAVEfmt ", 8);
    v = 16;                       memcpy(h + 16, &v, 4);
    uint16_t u = 1;               memcpy(h + 20, &u, 2);  // PCM
    u = 1;                        memcpy(h + 22, &u, 2);  // mono
    v = sample_rate;              memcpy(h + 24, &v, 4);
    v = byte_rate;                memcpy(h + 28, &v, 4);
    u = 2;                        memcpy(h + 32, &u, 2);  // block align
    u = 16;                       memcpy(h + 34, &u, 2);  // bits
    memcpy(h + 36, "data", 4);
    memcpy(h + 40, &pcm_bytes, 4);
}

namespace {

struct Chime {
    const char *name;
    TinyNote notes[4];
    size_t n;
};

// The reTerminal buzzer's two words, kept by ear: accept = one 2400 Hz blip
// for 60 ms; refuse = two 1800 Hz blips of 45 ms, 70 ms apart. A speaker can
// say more than a piezo, so "ask" (capture starts) rises and "error" falls.
constexpr Chime kChimes[] = {
    {"beep", {{2400, 60}}, 1},
    {"ok", {{2400, 60}}, 1},
    {"ack", {{2400, 60}}, 1},
    {"double", {{1800, 45}, {0, 70}, {1800, 45}}, 3},
    {"refuse", {{1800, 45}, {0, 70}, {1800, 45}}, 3},
    {"ask", {{1568, 60}, {0, 20}, {2093, 80}}, 3},
    {"error", {{880, 90}, {0, 30}, {587, 140}}, 3},
};

}  // namespace

size_t tiny_pcm_chime_notes(const char *name, TinyNote *out, size_t cap)
{
    if (!name || !out) return 0;
    for (const Chime &c : kChimes) {
        if (strcmp(c.name, name) != 0) continue;
        const size_t n = c.n < cap ? c.n : cap;
        memcpy(out, c.notes, n * sizeof(TinyNote));
        return n;
    }
    return 0;
}

namespace {

int read_int(const char **p)
{
    int v = 0;
    bool any = false;
    while (isdigit((unsigned char)**p)) {
        v = v * 10 + (**p - '0');
        if (v > 100000) v = 100000;
        ++*p;
        any = true;
    }
    return any ? v : -1;
}

void skip_ws(const char **p)
{
    while (**p == ' ' || **p == '\t') ++*p;
}

}  // namespace

size_t tiny_pcm_rtttl(const char *s, TinyNote *out, size_t cap, uint16_t max_hz)
{
    if (!s || !out || !cap) return 0;
    const char *c1 = strchr(s, ':');
    if (!c1) return 0;
    const char *c2 = strchr(c1 + 1, ':');
    if (!c2) return 0;

    // Defaults section: d=, o=, b= in any order.
    int def_d = 4, def_o = 6, bpm = 63;
    for (const char *p = c1 + 1; p < c2;) {
        skip_ws(&p);
        const char key = (char)tolower((unsigned char)*p);
        if ((key == 'd' || key == 'o' || key == 'b') && p[1] == '=') {
            p += 2;
            const int v = read_int(&p);
            if (key == 'd' && v > 0) def_d = v;
            if (key == 'o' && v >= 0) def_o = v;
            if (key == 'b' && v > 0) bpm = v;
        }
        while (p < c2 && *p != ',') ++p;
        if (p < c2) ++p;
    }
    const long whole_ms = 4L * 60000L / bpm;

    // Semitones above C within one octave.
    static const int kOffset[7] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
    size_t n = 0;
    const char *p = c2 + 1;
    while (*p && n < cap) {
        skip_ws(&p);
        if (!*p) break;
        int d = read_int(&p);
        if (d <= 0) d = def_d;
        const char letter = (char)tolower((unsigned char)*p);
        int semi = -1;
        if (letter >= 'a' && letter <= 'g') semi = kOffset[letter - 'a'];
        else if (letter != 'p') return n;  // not RTTTL past here: stop clean
        ++p;
        if (*p == '#') { if (semi >= 0) ++semi; ++p; }
        bool dotted = false;
        if (*p == '.') { dotted = true; ++p; }
        int o = read_int(&p);
        if (o < 0) o = def_o;
        if (*p == '.') { dotted = true; ++p; }

        long ms = whole_ms / d;
        if (dotted) ms += ms / 2;
        if (ms > 5000) ms = 5000;
        uint16_t hz = 0;
        if (semi >= 0) {
            // A4 = 440 Hz; semitone index relative to A4.
            const int idx = (o - 4) * 12 + semi - 9;
            const float f = 440.0f * powf(2.0f, (float)idx / 12.0f);
            hz = (f > 0 && f <= (float)max_hz) ? (uint16_t)(f + 0.5f) : 0;
        }
        out[n++] = TinyNote{hz, (uint16_t)ms};
        skip_ws(&p);
        if (*p == ',') ++p;
    }
    return n;
}

namespace {

constexpr int kSineBits = 8;  // 256-entry table
int16_t s_sine[1 << kSineBits];
bool s_sine_ready = false;

void sine_init()
{
    if (s_sine_ready) return;
    for (int i = 0; i < (1 << kSineBits); ++i)
        s_sine[i] = (int16_t)lrintf(32767.0f * sinf(6.2831853f * i / (1 << kSineBits)));
    s_sine_ready = true;
}

}  // namespace

void tiny_tone_start(TinyTone *t, uint32_t rate, const TinyNote &n, int16_t amp)
{
    sine_init();
    t->rate = rate;
    t->total = (uint32_t)((uint64_t)rate * n.ms / 1000);
    t->pos = 0;
    t->phase = 0;
    t->step = n.hz ? (uint32_t)(((uint64_t)n.hz << 32) / rate) : 0;
    // 4 ms edges: long enough that a speaker cone does not click, short
    // enough that a 45 ms blip still reads as a blip.
    uint32_t fade = rate * 4 / 1000;
    if (fade * 2 > t->total) fade = t->total / 2;
    t->fade = fade;
    t->amp = n.hz ? amp : 0;
}

size_t tiny_tone_render(TinyTone *t, int16_t *stereo, size_t frames)
{
    size_t w = 0;
    while (w < frames && t->pos < t->total) {
        int32_t v = 0;
        if (t->amp) {
            v = s_sine[t->phase >> (32 - kSineBits)];
            v = v * t->amp / 32767;
            const uint32_t from_end = t->total - 1 - t->pos;
            const uint32_t edge = t->pos < from_end ? t->pos : from_end;
            if (edge < t->fade) v = v * (int32_t)edge / (int32_t)t->fade;
            t->phase += t->step;
        }
        stereo[2 * w] = (int16_t)v;
        stereo[2 * w + 1] = (int16_t)v;
        ++t->pos;
        ++w;
    }
    return w;
}

TinyPcmStats tiny_pcm_stats(const int16_t *s, size_t n, size_t stride)
{
    TinyPcmStats st = {0, 0, 0};
    if (!s || !n || !stride) return st;
    double acc = 0;
    size_t nonzero = 0;
    size_t count = 0;
    for (size_t i = 0; i < n; i += stride, ++count) {
        int v = s[i];
        acc += (double)v * v;
        if (v < 0) v = -v;
        if (v > st.peak) st.peak = v;
        if (v) ++nonzero;
    }
    st.rms = (int)sqrt(acc / (double)count);
    st.nonzero_pct = (int)(nonzero * 100 / count);
    return st;
}
