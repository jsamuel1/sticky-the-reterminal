// tiny_dictate_util — the pure half of on-glass dictation (grammar v28).
//
// Dictation turns speech into text INSIDE an open composer: record, upload,
// POST /api/devices/transcribe, and the words are appended to the keyboard
// value for the human to correct and send through the composer's own route.
// Nothing here touches hardware or the network, so tests/host/dictate_host_test.sh
// compiles this exact header against the device's cJSON.
//
//   tiny_dictate_merge     append a transcript to a composer value: sanitized,
//                          space-joined, cut on a UTF-8 rune boundary, never
//                          past the buffer. Says whether it clipped.
//   tiny_dictate_utf8_pop  backspace one whole rune (a transcript carries é,
//                          ğ, ...; a byte-wise backspace leaves half a letter)
//   tiny_dictate_classify  one transcribe reply -> a verdict + the text
//   tiny_dictate_note      the one-line title the composer wears for a verdict
#pragma once
#include <stddef.h>
#include <string.h>

#include "cJSON.h"

typedef enum {
    TINY_DICTATE_MERGE_OK = 0,   // every word fit
    TINY_DICTATE_MERGE_CLIPPED,  // the head fit; the tail did not (buffer full)
    TINY_DICTATE_MERGE_EMPTY,    // nothing left after sanitizing
} tiny_dictate_merge_t;

// Verdicts for one dictation attempt. Names are the status `dictation_last`
// strings (tiny_dictate_class_name), so keep them stable.
typedef enum {
    TINY_DICTATE_OK = 0,        // text came back
    TINY_DICTATE_NOTHING,       // 422 / blank: no speech in the clip
    TINY_DICTATE_REFUSED,       // 400 / 404 "audio not found": server said no
    TINY_DICTATE_ROUTE_MISSING, // 404 without the route's own JSON: older app
    TINY_DICTATE_AUTH,          // 401: device token refused
    TINY_DICTATE_TRANSPORT,     // never reached the server (st <= 0)
    TINY_DICTATE_SERVER,        // any other status: the server failed
    TINY_DICTATE_MIC,           // capture failed or was refused (lock, no mic)
    TINY_DICTATE_UPLOAD,        // the recording never reached the media store
    TINY_DICTATE_BUSY,          // the mic is in use (an ask is in flight)
    TINY_DICTATE_UNAVAILABLE,   // no mic, or the server does not advertise it
    TINY_DICTATE_CLIPPED,       // transcript staging cap cut the server text
} tiny_dictate_class_t;

static inline const char *tiny_dictate_class_name(tiny_dictate_class_t c) {
    switch (c) {
        case TINY_DICTATE_OK:            return "ok";
        case TINY_DICTATE_NOTHING:       return "nothing-heard";
        case TINY_DICTATE_REFUSED:       return "refused";
        case TINY_DICTATE_ROUTE_MISSING: return "route-missing";
        case TINY_DICTATE_AUTH:          return "auth";
        case TINY_DICTATE_TRANSPORT:     return "transport";
        case TINY_DICTATE_SERVER:        return "server";
        case TINY_DICTATE_MIC:           return "mic";
        case TINY_DICTATE_UPLOAD:        return "upload";
        case TINY_DICTATE_BUSY:          return "busy";
        case TINY_DICTATE_UNAVAILABLE:   return "unavailable";
        case TINY_DICTATE_CLIPPED:       return "clipped";
    }
    return "?";
}

// Title-strip line for a verdict. <= 18 chars: the compact profile's title
// strip (FNK0104S 320 px, beside the glance cluster) cuts anything longer,
// and a cut verdict loses the half that says what to do. The draft itself
// stays visible in the value box, so the note need not say it survived.
static inline const char *tiny_dictate_note(tiny_dictate_class_t c) {
    switch (c) {
        case TINY_DICTATE_OK:            return "";
        case TINY_DICTATE_NOTHING:       return "didn't catch that";
        case TINY_DICTATE_REFUSED:       return "clip refused";
        case TINY_DICTATE_ROUTE_MISSING: return "no dictation here";
        case TINY_DICTATE_AUTH:          return "not signed in";
        case TINY_DICTATE_TRANSPORT:     return "offline - type it";
        case TINY_DICTATE_SERVER:        return "transcribe failed";
        case TINY_DICTATE_MIC:           return "mic unavailable";
        case TINY_DICTATE_UPLOAD:        return "upload failed";
        case TINY_DICTATE_BUSY:          return "mic busy - wait";
        case TINY_DICTATE_UNAVAILABLE:   return "no dictation here";
        case TINY_DICTATE_CLIPPED:       return "field full - check";
    }
    return "";
}

// The other title lines dictation paints, under the same 18-char rule.
#define TINY_DICTATE_TITLE_LISTENING "listening..."
#define TINY_DICTATE_TITLE_CHECK     "check, then OK"
#define TINY_DICTATE_TITLE_CLIPPED   "field full - check"
#define TINY_DICTATE_TITLE_MAX       18

// Length of the longest prefix of s[0..len) that ends on a rune boundary.
static inline size_t tiny_dictate_utf8_floor(const char *s, size_t len) {
    if (len == 0) return 0;
    // Walk back over continuation bytes to the lead byte of the last rune.
    size_t i = len;
    size_t back = 0;
    while (i > 0 && back < 4 && (((unsigned char)s[i - 1]) & 0xC0) == 0x80) {
        --i;
        ++back;
    }
    if (i == 0) return 0;  // only continuation bytes: nothing whole
    const unsigned char lead = (unsigned char)s[i - 1];
    size_t need = 1;
    if (lead >= 0xF0) need = 4;
    else if (lead >= 0xE0) need = 3;
    else if (lead >= 0xC0) need = 2;
    // The last rune is whole when its continuation count matches the lead.
    return (back + 1 == need) ? len : i - 1;
}

// Remove the last whole rune of a NUL-terminated buffer. No-op when empty.
static inline void tiny_dictate_utf8_pop(char *s) {
    size_t n = strlen(s);
    if (!n) return;
    size_t i = n - 1;
    while (i > 0 && (((unsigned char)s[i]) & 0xC0) == 0x80) --i;
    s[i] = 0;
}

// Validate one UTF-8 rune and report its byte length. Invalid bytes are
// separators at this trust boundary, never copied into the editable draft.
static inline bool tiny_dictate_utf8_valid(const unsigned char *s, size_t n,
                                           size_t *rune_len) {
    const unsigned char c = s[0];
    size_t need = 0;
    if (c < 0x80) need = 1;
    else if (c >= 0xC2 && c <= 0xDF) need = 2;
    else if (c >= 0xE0 && c <= 0xEF) need = 3;
    else if (c >= 0xF0 && c <= 0xF4) need = 4;
    else return false;
    if (need > n) return false;
    for (size_t i = 1; i < need; ++i)
        if ((s[i] & 0xC0) != 0x80) return false;
    if (need == 3 && c == 0xE0 && s[1] < 0xA0) return false;  // overlong
    if (need == 3 && c == 0xED && s[1] >= 0xA0) return false; // surrogate
    if (need == 4 && c == 0xF0 && s[1] < 0x90) return false;  // overlong
    if (need == 4 && c == 0xF4 && s[1] >= 0x90) return false; // > U+10FFFF
    if (rune_len) *rune_len = need;
    return true;
}

// Append `spoken` to the composer value `buf` (capacity `cap`, NUL included).
// Control bytes become spaces, whitespace runs collapse to one space, ends are
// trimmed; a single space joins it to existing text that does not already end
// in one. When it does not all fit, the cut lands on a rune boundary and the
// clipped verdict is returned — the caller says so on the glass.
static inline tiny_dictate_merge_t tiny_dictate_merge(char *buf, size_t cap,
                                                      const char *spoken) {
    if (!buf || cap == 0) return TINY_DICTATE_MERGE_EMPTY;
    if (!spoken) return TINY_DICTATE_MERGE_EMPTY;
    size_t len = strlen(buf);
    if (len >= cap) {  // a caller bug, not a reason to write past the end
        buf[cap - 1] = 0;
        len = cap - 1;
    }
    const size_t start = len;
    bool any = false;
    bool pending_space = len > 0 && buf[len - 1] != ' ';
    bool clipped = false;
    const unsigned char *p = (const unsigned char *)spoken;
    const size_t spoken_len = strlen(spoken);
    size_t pos = 0;
    while (pos < spoken_len) {
        const unsigned char ch = p[pos];
        const bool ws = ch < 0x20 || ch == 0x7F || ch == ' ';
        if (ws) {
            if (any) pending_space = true;  // collapse; leading ones dropped
            ++pos;
            continue;
        }
        size_t rune = 0;
        if (!tiny_dictate_utf8_valid(p + pos, spoken_len - pos, &rune)) {
            if (any) pending_space = true;  // invalid bytes become one space
            ++pos;
            continue;
        }
        any = true;  // there IS something to say, whether or not it fits
        const size_t join = pending_space ? 1 : 0;
        if (len + join + rune >= cap) { clipped = true; break; }
        if (pending_space) buf[len++] = ' ';
        memcpy(buf + len, p + pos, rune);
        len += rune;
        pos += rune;
        pending_space = false;
    }
    // Every rune was admitted atomically, so len already ends on a boundary.
    while (len > start && buf[len - 1] == ' ') --len;
    buf[len] = 0;
    if (!any) return TINY_DICTATE_MERGE_EMPTY;
    if (len == start) return TINY_DICTATE_MERGE_CLIPPED;  // no room at all
    return clipped ? TINY_DICTATE_MERGE_CLIPPED : TINY_DICTATE_MERGE_OK;
}

// Classify one /api/devices/transcribe reply. `st` is the HTTP status, or
// <= 0 when the request never left the device. On TINY_DICTATE_OK, `text`
// holds the transcript (truncated on a rune boundary to `cap`).
static inline tiny_dictate_class_t tiny_dictate_classify(int st, const char *body,
                                                         char *text, size_t cap) {
    if (text && cap) text[0] = 0;
    if (st <= 0) return TINY_DICTATE_TRANSPORT;
    cJSON *o = body ? cJSON_Parse(body) : NULL;
    const cJSON *t = o ? cJSON_GetObjectItem(o, "text") : NULL;
    const cJSON *e = o ? cJSON_GetObjectItem(o, "error") : NULL;
    tiny_dictate_class_t c;
    if (st == 200) {
        if (cJSON_IsString(t) && t->valuestring && t->valuestring[0] && text && cap) {
            // Copy complete, valid UTF-8 runes. Invalid input bytes become
            // spaces, never malformed text in the composer.
            const unsigned char *src = (const unsigned char *)t->valuestring;
            const size_t n = strlen(t->valuestring);
            size_t read = 0, keep = 0;
            while (read < n) {
                size_t rune = 0;
                if (!tiny_dictate_utf8_valid(src + read, n - read, &rune)) {
                    if (keep + 1 >= cap) break;
                    text[keep++] = ' ';
                    ++read;
                    continue;
                }
                if (keep + rune >= cap) break;
                memcpy(text + keep, src + read, rune);
                keep += rune;
                read += rune;
            }
            // The input is clipped only when valid unread runes remain. Invalid
            // trailing bytes were intentionally sanitized, not capacity loss.
            bool transcript_clipped = false;
            for (size_t rest = read; rest < n;) {
                size_t rune = 0;
                if (tiny_dictate_utf8_valid(src + rest, n - rest, &rune)) {
                    transcript_clipped = true;
                    break;
                }
                ++rest;
            }
            text[keep] = 0;
            // Blank after trimming is "nothing heard", not success.
            bool blank = true;
            for (size_t i = 0; i < keep; ++i)
                if ((unsigned char)text[i] > 0x20) { blank = false; break; }
            c = blank ? TINY_DICTATE_NOTHING
                      : transcript_clipped ? TINY_DICTATE_CLIPPED
                                           : TINY_DICTATE_OK;
            if (blank) text[0] = 0;
        } else {
            c = TINY_DICTATE_NOTHING;
        }
    } else if (st == 422) {
        c = TINY_DICTATE_NOTHING;
    } else if (st == 401) {
        c = TINY_DICTATE_AUTH;
    } else if (st == 400) {
        c = TINY_DICTATE_REFUSED;
    } else if (st == 404) {
        // The route answers 404 with its own JSON sentence when the audio is
        // not this owner's. Anything else (an HTML page, a bare 404) means the
        // app in front of the worker does not serve the route at all.
        c = cJSON_IsString(e) ? TINY_DICTATE_REFUSED : TINY_DICTATE_ROUTE_MISSING;
    } else {
        c = TINY_DICTATE_SERVER;
    }
    if (o) cJSON_Delete(o);
    return c;
}

// Does a heartbeat reply's `features` array name `want`? -1 = the reply has
// no features array (older backend, or not asked): keep what was known.
static inline int tiny_dictate_feature(const char *hb_reply, const char *want) {
    cJSON *o = hb_reply ? cJSON_Parse(hb_reply) : NULL;
    if (!o) return -1;
    const cJSON *f = cJSON_GetObjectItem(o, "features");
    int r = -1;
    if (cJSON_IsArray(f)) {
        r = 0;
        const cJSON *it = NULL;
        cJSON_ArrayForEach(it, f)
            if (cJSON_IsString(it) && it->valuestring && !strcmp(it->valuestring, want)) r = 1;
    }
    cJSON_Delete(o);
    return r;
}
