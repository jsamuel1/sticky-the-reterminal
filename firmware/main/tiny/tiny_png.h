#pragma once

// tiny_png — the screenshot's PNG writer, pure (no ESP-IDF) so the host test
// encodes the same bytes the device uploads. Uncompressed: zlib STORED
// deflate blocks, filter 0 on every row — deterministic, no library.
//
// Two shapes:
//   gray2    — bit depth 2, colour type 0: the e-ink canvas exactly (levels
//              0..3 -> 00/55/AA/FF). Byte-identical to every screenshot
//              before grammar v16.
//   indexed8 — bit depth 8, colour type 3 + PLTE: a colour board, one byte
//              per pixel = lcd_palette index (accent << 2 | gray), PLTE =
//              the theme's colours, so the PNG shows what the glass shows.
//
// The caller supplies crc32 (device: esp_rom_crc32_le, host: a bitwise one)
// and the raw scanlines (each prefixed by its filter byte 0).

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace tiny_png {

using Crc32Fn = uint32_t (*)(uint32_t crc, const uint8_t *buf, size_t len);

inline void put_u32(uint8_t *p, uint32_t v) {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

// Bytes needed for an image whose raw scanlines total `raw_len` with a
// `plte_entries`-entry palette (0 = none).
inline size_t capacity(size_t raw_len, int plte_entries) {
    const size_t nblocks = (raw_len + 65534) / 65535;
    const size_t zlen = 2 + raw_len + nblocks * 5 + 4;
    const size_t plte = plte_entries > 0 ? 12 + 3 * static_cast<size_t>(plte_entries) : 0;
    return 8 + 25 + plte + (zlen + 12) + 12;  // sig + IHDR + PLTE + IDAT + IEND
}

// One chunk at dst: length + type + data + crc32(type || data).
inline size_t chunk(uint8_t *dst, const char *type, const uint8_t *data, size_t len,
                    Crc32Fn crc32) {
    put_u32(dst, static_cast<uint32_t>(len));
    std::memcpy(dst + 4, type, 4);
    if (len) std::memcpy(dst + 8, data, len);
    put_u32(dst + 8 + len, crc32(0, dst + 4, len + 4));
    return len + 12;
}

// Encode into `png` (>= capacity()). `plte` = 3*plte_entries RGB bytes or
// null. Returns the PNG length.
inline size_t encode(uint8_t *png, uint32_t w, uint32_t h, uint8_t bit_depth,
                     uint8_t colour_type, const uint8_t *plte, int plte_entries,
                     const uint8_t *raw, size_t raw_len, Crc32Fn crc32) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::memcpy(png, sig, 8);
    size_t n = 8;

    uint8_t ihdr[13];
    put_u32(ihdr, w);
    put_u32(ihdr + 4, h);
    ihdr[8] = bit_depth;
    ihdr[9] = colour_type;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;  // deflate, adaptive filter, no interlace
    n += chunk(png + n, "IHDR", ihdr, 13, crc32);
    if (plte && plte_entries > 0)
        n += chunk(png + n, "PLTE", plte, 3 * static_cast<size_t>(plte_entries), crc32);

    uint32_t a = 1, b = 0;  // adler32 over raw
    for (size_t i = 0; i < raw_len; ++i) {
        a = (a + raw[i]) % 65521;
        b = (b + a) % 65521;
    }
    // IDAT payload assembled in place after its 8-byte header slot.
    uint8_t *z = png + n + 8;
    size_t zi = 0;
    z[zi++] = 0x78;
    z[zi++] = 0x01;  // zlib: deflate, 32K window, no dict
    size_t off = 0;
    while (off < raw_len) {
        const size_t take = raw_len - off > 65535 ? 65535 : raw_len - off;
        z[zi++] = (off + take == raw_len) ? 1 : 0;  // BFINAL on the last block
        z[zi++] = static_cast<uint8_t>(take & 0xFF);
        z[zi++] = static_cast<uint8_t>(take >> 8);
        z[zi++] = static_cast<uint8_t>(~take & 0xFF);
        z[zi++] = static_cast<uint8_t>((~take >> 8) & 0xFF);
        std::memcpy(z + zi, raw + off, take);
        zi += take;
        off += take;
    }
    put_u32(z + zi, (b << 16) | a);
    zi += 4;
    put_u32(png + n, static_cast<uint32_t>(zi));
    std::memcpy(png + n + 4, "IDAT", 4);
    put_u32(png + n + 8 + zi, crc32(0, png + n + 4, zi + 4));
    n += zi + 12;

    n += chunk(png + n, "IEND", nullptr, 0, crc32);
    return n;
}

}  // namespace tiny_png
