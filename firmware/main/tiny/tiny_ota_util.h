// tiny_ota_util — the pure half of tiny_ota.cpp: board stamp + URL/sha guards.
//
// Header-only and free of ESP-IDF on purpose: tests/host/ota_host_test.sh
// compiles THIS file into a host binary, so the bytes the board checks before
// it erases an OTA slot are the bytes the test checks. tools/ota_push.py
// carries a Python twin of parse_image(); the host test runs both on the same
// fixtures so they cannot drift apart.
//
// Why a stamp: the three board builds share one project name and one app
// descriptor, so before this nothing in an app image said which board it was
// for. Staging a reTerminal image on an FNK0104S boots the wrong pin map.
//
// Where it lives: board_caps.cpp places a tiny_board_stamp_t in section
// .rodata_custom_desc. IDF's linker script puts that section SECOND in the
// first DROM segment, straight after the 256-byte esp_app_desc_t
// (components/esp_system/ld/esp32s3/sections.ld.in: "Should be the second.
// Custom app version info."). The image file therefore reads:
//
//   0    esp_image_header_t   (24 B, byte 0 = 0xE9)
//   24   esp_image_segment_header_t (8 B)
//   32   esp_app_desc_t       (256 B, magic 0xABCD5432 LE, project_name at +48)
//   288  tiny_board_stamp_t   (32 B, magic "TINYBRD1", board id NUL-padded)
//
// Those 320 bytes arrive in the first HTTP read; tiny_ota checks them BEFORE
// esp_ota_begin, so a foreign image never touches the inactive slot.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TINY_BOARD_STAMP_MAGIC "TINYBRD1"  // 8 bytes, no NUL in the image

typedef struct {
    char magic[8];    // TINY_BOARD_STAMP_MAGIC
    char board[16];   // board_caps id, NUL-padded ("reterminal", "fnk0104s", ...)
    uint8_t rsv[8];   // zero
} tiny_board_stamp_t;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
static_assert(sizeof(tiny_board_stamp_t) == 32, "stamp is 32 bytes on the wire");

namespace tiny_ota_util {

constexpr size_t kImgHdr = 24;           // esp_image_header_t
constexpr size_t kSegHdr = 8;            // esp_image_segment_header_t
constexpr size_t kDescOff = kImgHdr + kSegHdr;     // 32
constexpr size_t kDescLen = 256;         // esp_app_desc_t
constexpr size_t kProjOff = kDescOff + 48;         // esp_app_desc_t.project_name
constexpr size_t kStampOff = kDescOff + kDescLen;  // 288
constexpr size_t kHeadLen = kStampOff + sizeof(tiny_board_stamp_t);  // 320
constexpr uint32_t kDescMagic = 0xABCD5432u;

enum Verdict {
    IMG_OK = 0,
    IMG_TRUNCATED,    // fewer than kHeadLen bytes
    IMG_NOT_ESP,      // no 0xE9 header or no app descriptor at offset 32
    IMG_UNSTAMPED,    // an app image without a board stamp (pre-0.32 build)
    IMG_BAD_STAMP,    // stamp magic present, board field malformed
    IMG_WRONG_PROJECT,
    IMG_WRONG_BOARD,
};

inline const char *verdict_name(Verdict v) {
    switch (v) {
        case IMG_OK: return "ok";
        case IMG_TRUNCATED: return "truncated";
        case IMG_NOT_ESP: return "not an ESP app image";
        case IMG_UNSTAMPED: return "unstamped (no board id)";
        case IMG_BAD_STAMP: return "malformed board stamp";
        case IMG_WRONG_PROJECT: return "different project";
        case IMG_WRONG_BOARD: return "built for another board";
    }
    return "?";
}

// Parse the first `len` bytes of an app image. On IMG_OK / IMG_WRONG_BOARD /
// IMG_WRONG_PROJECT the image's board id is copied to board_out (NUL-terminated,
// cap >= 16). `project` and `want_board` may be NULL to skip that comparison.
inline Verdict parse_image(const uint8_t *p, size_t len, const char *project,
                           const char *want_board, char *board_out, size_t board_cap) {
    if (board_out && board_cap) board_out[0] = 0;
    if (len < kHeadLen) return IMG_TRUNCATED;
    if (p[0] != 0xE9) return IMG_NOT_ESP;
    const uint32_t m = (uint32_t)p[kDescOff] | (uint32_t)p[kDescOff + 1] << 8 |
                       (uint32_t)p[kDescOff + 2] << 16 | (uint32_t)p[kDescOff + 3] << 24;
    if (m != kDescMagic) return IMG_NOT_ESP;
    const tiny_board_stamp_t *s = (const tiny_board_stamp_t *)(p + kStampOff);
    if (memcmp(s->magic, TINY_BOARD_STAMP_MAGIC, 8) != 0) return IMG_UNSTAMPED;
    // board: 1..15 chars of [a-z0-9_-], then NUL padding to the end.
    size_t n = 0;
    while (n < sizeof s->board && s->board[n]) {
        const char c = s->board[n];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-'))
            return IMG_BAD_STAMP;
        ++n;
    }
    if (n == 0 || n >= sizeof s->board) return IMG_BAD_STAMP;
    for (size_t i = n; i < sizeof s->board; ++i)
        if (s->board[i]) return IMG_BAD_STAMP;
    char got[sizeof s->board];
    memcpy(got, s->board, n);
    got[n] = 0;
    if (board_out && board_cap) {
        const size_t k = n < board_cap - 1 ? n : board_cap - 1;
        memcpy(board_out, got, k);
        board_out[k] = 0;
    }
    if (project) {
        const char *pn = (const char *)(p + kProjOff);
        if (strnlen(pn, 32) != strlen(project) || memcmp(pn, project, strlen(project)) != 0)
            return IMG_WRONG_PROJECT;
    }
    if (want_board && strcmp(got, want_board) != 0) return IMG_WRONG_BOARD;
    return IMG_OK;
}

// The `ota <url> <sha256>` form accepts exactly one artifact origin: the
// media store the CI pusher uploads to. Prefix match on the full scheme+host+
// path, then a single path segment of [A-Za-z0-9._-] (the worker's own media
// key alphabet), no "..", bounded length. Anything else is refused.
constexpr const char kMediaPrefix[] = "https://plugin.tiny.technology/media/";

inline bool media_url_ok(const char *u) {
    if (!u) return false;
    const size_t pl = sizeof kMediaPrefix - 1;
    if (strncmp(u, kMediaPrefix, pl) != 0) return false;
    const char *k = u + pl;
    size_t n = 0;
    for (; k[n]; ++n) {
        const char c = k[n];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-'))
            return false;
        if (n > 128) return false;
    }
    if (n == 0 || k[0] == '.') return false;
    if (strstr(k, "..")) return false;
    return true;
}

inline bool sha256_hex_ok(const char *h) {
    if (!h) return false;
    for (int i = 0; i < 64; ++i) {
        const char c = h[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            return false;
    }
    return h[64] == 0;
}

// Dispatch for the `ota` verb: a scheme or a second argument selects the
// direct-URL form. This sends malformed two-argument URLs and non-HTTPS
// schemes through the URL guard instead of silently treating arg 1 as a
// channel and ignoring arg 2.
inline bool ota_args_are_direct(const char *a, int argc) {
    return argc >= 2 || (a && strstr(a, "://") != nullptr);
}

}  // namespace tiny_ota_util
#endif
