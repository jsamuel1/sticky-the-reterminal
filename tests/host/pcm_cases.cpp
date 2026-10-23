// pcm host test — the SHIPPED firmware/main/tiny_pcm.cpp: WAV header bytes,
// chime vocabulary, RTTTL parsing, click-free tone edges, stats.
#include "tiny_pcm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)

static uint32_t u32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint16_t u16(const uint8_t *p) { uint16_t v; memcpy(&v, p, 2); return v; }

static void wav_header()
{
    uint8_t h[44];
    tiny_pcm_wav_header(h, 16000, 32000);
    CHECK(!memcmp(h, "RIFF", 4) && !memcmp(h + 8, "WAVEfmt ", 8) && !memcmp(h + 36, "data", 4));
    CHECK(u32(h + 4) == 36 + 32000);
    CHECK(u16(h + 20) == 1 && u16(h + 22) == 1);
    CHECK(u32(h + 24) == 16000 && u32(h + 28) == 32000);
    CHECK(u16(h + 32) == 2 && u16(h + 34) == 16);
    CHECK(u32(h + 40) == 32000);
}

static void chimes()
{
    TinyNote n[8];
    CHECK(tiny_pcm_chime_notes("beep", n, 8) == 1 && n[0].hz == 2400 && n[0].ms == 60);
    CHECK(tiny_pcm_chime_notes("double", n, 8) == 3);
    CHECK(n[0].hz == 1800 && n[0].ms == 45 && n[1].hz == 0 && n[1].ms == 70 && n[2].ms == 45);
    CHECK(tiny_pcm_chime_notes("ask", n, 8) == 3);
    CHECK(tiny_pcm_chime_notes("ack", n, 8) == 1);
    CHECK(tiny_pcm_chime_notes("error", n, 8) == 3 && n[0].hz > n[2].hz);
    CHECK(tiny_pcm_chime_notes("nope", n, 8) == 0);
    CHECK(tiny_pcm_chime_notes(nullptr, n, 8) == 0);
    CHECK(tiny_pcm_chime_notes("double", n, 2) == 2);  // capped, not overrun
}

static void rtttl()
{
    TinyNote n[16];
    // b=120: whole = 2000 ms. d=8 -> 250 ms. a5 = 880 Hz. c6 = 1047 Hz.
    size_t k = tiny_pcm_rtttl("t:d=8,o=5,b=120:a,c6,p,4e6.,16g#5", n, 16, 7000);
    CHECK(k == 5);
    CHECK(n[0].hz == 880 && n[0].ms == 250);
    CHECK(n[1].hz == 1047 && n[1].ms == 250);
    CHECK(n[2].hz == 0 && n[2].ms == 250);
    CHECK(n[3].hz == 1319 && n[3].ms == 750);  // dotted quarter
    CHECK(n[4].hz == 831 && n[4].ms == 125);   // g#5
    // Defaults: d=4, o=6, b=63.
    k = tiny_pcm_rtttl("x::c", n, 16, 7000);
    CHECK(k == 1 && n[0].hz == 1047 && n[0].ms == 4 * 60000 / 63 / 4);
    // Above Nyquist margin -> rest.
    k = tiny_pcm_rtttl("x:o=8:b", n, 16, 7000);
    CHECK(k == 1 && n[0].hz == 0);
    CHECK(tiny_pcm_rtttl("ack", n, 16, 7000) == 0);
    CHECK(tiny_pcm_rtttl("x:d=4:c,c,c,c", n, 2, 7000) == 2);
    CHECK(tiny_pcm_rtttl("x:d=4:c,zz", n, 16, 7000) == 1);  // garbage stops clean
}

static void tones()
{
    TinyTone t;
    tiny_tone_start(&t, 16000, TinyNote{2400, 60}, 8000);
    static int16_t buf[2 * 2000];
    size_t got = 0, w;
    while ((w = tiny_tone_render(&t, buf + 2 * got, 100)) > 0) got += w;
    CHECK(got == 960);  // 60 ms at 16 kHz
    CHECK(buf[0] == 0);  // starts at zero: no click
    CHECK(abs(buf[2 * (got - 1)]) < 200);  // ends near zero
    int peak = 0;
    for (size_t i = 0; i < got; ++i) {
        CHECK(buf[2 * i] == buf[2 * i + 1]);
        if (abs(buf[2 * i]) > peak) peak = abs(buf[2 * i]);
    }
    CHECK(peak > 7000 && peak <= 8000);
    // Zero crossings ~ 2 per cycle: 2400 Hz * 60 ms = 144 cycles.
    int zc = 0;
    for (size_t i = 1; i < got; ++i)
        if ((buf[2 * i - 2] < 0) != (buf[2 * i] < 0)) ++zc;
    CHECK(zc > 270 && zc < 300);
    // Rest renders silence of the right length.
    tiny_tone_start(&t, 16000, TinyNote{0, 10}, 8000);
    got = tiny_tone_render(&t, buf, 2000);
    CHECK(got == 160);
    bool silent = true;
    for (size_t i = 0; i < 2 * got; ++i) silent &= buf[i] == 0;
    CHECK(silent);
}

static void stats()
{
    int16_t s[8] = {100, 5, -100, 5, 0, 5, 0, 5};
    TinyPcmStats st = tiny_pcm_stats(s, 8, 2);  // left slot only
    CHECK(st.peak == 100 && st.rms == 70 && st.nonzero_pct == 50);
    st = tiny_pcm_stats(s, 0, 1);
    CHECK(st.rms == 0 && st.peak == 0);
}

int main()
{
    wav_header();
    chimes();
    rtttl();
    tones();
    stats();
    if (fails) { printf("%d failure(s)\n", fails); return 1; }
    puts("pcm cases: ok");
    return 0;
}
