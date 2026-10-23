// tiny_pair_util — the pure half of on-device pairing (tiny_pair.cpp).
//
// Header-only and free of ESP-IDF on purpose: tests/host/pair_host_test.sh
// compiles THIS file into a plain host binary, so the bytes the board hashes,
// encodes and sends are the bytes the test checks. Nothing here allocates,
// logs or touches a radio.
//
// Contract mirrored from tiny-tech 0.21.1 dist/enroll.js pairDevice():
//   secret     = randomBytes(32).toString('base64url')      (43 chars, no '=')
//   secretHash = sha256(secret STRING, utf-8).hex             (64 lowercase hex)
//   start      POST {api}/api/devices/pair/start {..claim, secretHash} -> {code, ttl_s}
//   poll       POST {api}/api/devices/pair/poll  {code, secret}
//              -> {status: approved|denied|expired|claimed|pending, ...}
//              404 = the code vanished (registry reset): start again
//   owner URL  {api without trailing /}/devices/pair?code=<code>
//   display    code.slice(0,3) + ' ' + code.slice(3)
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace tiny_pair_util {

// ---------- SHA-256 (FIPS 180-4) ----------
// Small and portable rather than mbedtls: the host test must exercise the
// exact routine the firmware runs, and hashing 43 bytes once per code costs
// nothing worth accelerating.
inline constexpr uint32_t kK[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

inline uint32_t ror32(uint32_t x, int r) { return (x >> r) | (x << (32 - r)); }

inline void sha256_block(uint32_t h[8], const uint8_t *p) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + S1 + ch + kK[i] + w[i];
        const uint32_t S0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        const uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

inline void sha256(const void *data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const uint8_t *p = static_cast<const uint8_t *>(data);
    size_t n = len;
    while (n >= 64) { sha256_block(h, p); p += 64; n -= 64; }
    uint8_t tail[128] = {0};
    memcpy(tail, p, n);
    tail[n] = 0x80;
    const size_t tl = (n < 56) ? 64 : 128;
    const uint64_t bits = (uint64_t)len * 8;
    for (int i = 0; i < 8; ++i) tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha256_block(h, tail);
    if (tl == 128) sha256_block(h, tail + 64);
    for (int i = 0; i < 8; ++i) {
        out[4 * i] = (uint8_t)(h[i] >> 24); out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8); out[4 * i + 3] = (uint8_t)h[i];
    }
}

// Lowercase hex; out needs 2*n+1.
inline void hex(const uint8_t *in, size_t n, char *out) {
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { out[2 * i] = d[in[i] >> 4]; out[2 * i + 1] = d[in[i] & 15]; }
    out[2 * n] = 0;
}

// ---------- base64url, no padding (Node Buffer 'base64url') ----------
inline constexpr size_t b64url_len(size_t n) { return (4 * n + 2) / 3; }

// Returns chars written (excluding NUL), or -1 when cap is too small.
inline int b64url(const uint8_t *in, size_t n, char *out, size_t cap) {
    static const char a[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    const size_t need = b64url_len(n);
    if (!out || cap < need + 1) return -1;
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 |
                           (i + 1 < n ? (uint32_t)in[i + 1] << 8 : 0) |
                           (i + 2 < n ? (uint32_t)in[i + 2] : 0);
        out[o++] = a[(v >> 18) & 63];
        out[o++] = a[(v >> 12) & 63];
        if (i + 1 < n) out[o++] = a[(v >> 6) & 63];
        if (i + 2 < n) out[o++] = a[v & 63];
    }
    out[o] = 0;
    return (int)o;
}

// ---------- the pairing contract ----------
inline constexpr size_t kSecretBytes = 32;
inline constexpr size_t kSecretChars = b64url_len(kSecretBytes);  // 43
inline constexpr size_t kCodeMax = 16;

// secretHash = sha256hex(secret string). out needs 65.
inline void secret_hash(const char *secret, char out[65]) {
    uint8_t d[32];
    sha256(secret, strlen(secret), d);
    hex(d, 32, out);
}

// A code the server hands us lands in a URL, a QR, a log line and two JSON
// replies built with snprintf. Accept only what is safe in all of them:
// 4..16 ASCII letters/digits. Anything else is a malformed start reply.
inline bool code_ok(const char *code) {
    if (!code) return false;
    const size_t n = strlen(code);
    if (n < 4 || n > kCodeMax) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = code[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')))
            return false;
    }
    return true;
}

// "ABC123" -> "ABC 123" (enroll.js: slice(0,3) + ' ' + slice(3)).
inline bool code_split(const char *code, char *out, size_t cap) {
    const size_t n = strlen(code);
    if (n <= 3) return snprintf(out, cap, "%s", code) < (int)cap;
    return snprintf(out, cap, "%.3s %s", code, code + 3) < (int)cap;
}

// {api}/devices/pair?code=<code>, api's trailing '/' dropped. false = truncated.
inline bool pair_url(const char *api, const char *code, char *out, size_t cap) {
    size_t n = strlen(api);
    while (n > 0 && api[n - 1] == '/') --n;
    const int w = snprintf(out, cap, "%.*s/devices/pair?code=%s", (int)n, api, code);
    return w > 0 && (size_t)w < cap;
}

// JSON string literal (quotes included) appended at out[*o]. false = no room.
inline bool json_str(const char *s, char *out, size_t cap, size_t *o) {
    auto put = [&](char c) { if (*o + 1 >= cap) return false; out[(*o)++] = c; return true; };
    if (!put('"')) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            if (!put('\\') || !put((char)*p)) return false;
        } else if (*p < 0x20) {
            char u[7];
            snprintf(u, sizeof u, "\\u%04x", *p);
            for (const char *q = u; *q; ++q) if (!put(*q)) return false;
        } else if (!put((char)*p)) {
            return false;
        }
    }
    if (!put('"')) return false;
    out[*o] = 0;
    return true;
}

// The tiny_config_merge_json body for an approved code:
//   {"device_id":<id>,"token":<device_token>[,"name":<name>]}
// Identity only — networks stay as they are (merge upserts, never clears).
// false = empty id/token or no room; out is then not a usable payload.
inline bool merge_json(const char *device_id, const char *token, const char *name,
                       char *out, size_t cap) {
    if (!device_id || !*device_id || !token || !*token || !out || cap < 2) return false;
    size_t o = 0;
    auto lit = [&](const char *s) {
        for (; *s; ++s) { if (o + 1 >= cap) return false; out[o++] = *s; }
        out[o] = 0;
        return true;
    };
    if (!lit("{\"device_id\":") || !json_str(device_id, out, cap, &o)) return false;
    if (!lit(",\"token\":") || !json_str(token, out, cap, &o)) return false;
    if (name && *name && (!lit(",\"name\":") || !json_str(name, out, cap, &o))) return false;
    return lit("}");
}

// What one poll reply means for the loop.
enum class Poll {
    kApproved,  // 2xx + status approved + device_token: merge, reboot
    kPending,   // 2xx, still waiting for the owner: poll again in 3 s
    kDenied,    // the owner said no: stop, offer a new code
    kRestart,   // expired / claimed / 404: this code is dead, get a new one
    kRetry,     // transport, 5xx, 4xx, malformed: same code, back off
};

// http <= 0 = no response at all. status may be NULL (unparsable body).
inline Poll classify(int http, const char *status, bool has_token) {
    if (http == 404) return Poll::kRestart;
    const bool ok = http >= 200 && http < 300;
    if (status) {
        if (!strcmp(status, "denied")) return Poll::kDenied;
        if (!strcmp(status, "expired") || !strcmp(status, "claimed")) return Poll::kRestart;
        // approved without a token is not success: the token is shown once,
        // and the next poll answers "claimed" if someone else took it.
        if (ok && !strcmp(status, "approved")) return has_token ? Poll::kApproved : Poll::kRetry;
    }
    return ok ? Poll::kPending : Poll::kRetry;
}

// 3 s -> 6 -> 12 -> 24 -> 30 s ceiling.
inline uint32_t backoff_ms(uint32_t cur) {
    if (cur < 3000) return 3000;
    return cur >= 15000 ? 30000 : cur * 2;
}

}  // namespace tiny_pair_util
