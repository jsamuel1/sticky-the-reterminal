#!/bin/bash
# lcd_palette host test — compiles the SHIPPED firmware/main/lcd_palette.h and
# tiny/tiny_png.h with the shipped canvas.cpp/font.cpp: the accent plane, the
# theme palette, gray4+accent4 -> big-endian RGB565 strips, the dirty band, and
# the e-ink invariant (no plane = gray bytes unchanged). Then an independent
# decoder (python3 zlib, no PIL) reads the PNGs the shipped encoder wrote:
# chunk CRCs, IHDR shape, and that indexed pixels resolve to palette colours.
set -euo pipefail
cd "$(dirname "$0")"
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
c++ -std=c++17 -Wall -Wextra -Werror -I shim -I ../../firmware/main \
    ../../firmware/main/canvas.cpp ../../firmware/main/font.cpp \
    lcd_palette_cases.cpp -o "$out/lcd_palette_test"
"$out/lcd_palette_test" "$out"
python3 - "$out" <<'EOF'
import struct, sys, zlib
d = sys.argv[1]

def read(path):
    b = open(path, "rb").read()
    assert b[:8] == b"\x89PNG\r\n\x1a\n", "signature"
    i, chunks = 8, []
    while i < len(b):
        n, = struct.unpack(">I", b[i:i+4]); t = b[i+4:i+8]; data = b[i+8:i+8+n]
        crc, = struct.unpack(">I", b[i+8+n:i+12+n])
        assert zlib.crc32(t + data) & 0xffffffff == crc, f"crc {t}"
        chunks.append((t, data)); i += 12 + n
    assert chunks[0][0] == b"IHDR" and chunks[-1][0] == b"IEND"
    w, h, depth, ctype = struct.unpack(">IIBB", chunks[0][1][:10])
    plte = dict(chunks).get(b"PLTE")
    raw = zlib.decompress(b"".join(c for t, c in chunks if t == b"IDAT"))
    stride = (w * depth + 7) // 8
    assert len(raw) == h * (stride + 1), "raw length"
    rows = [raw[y*(stride+1)+1:(y+1)*(stride+1)] for y in range(h)]
    assert all(raw[y*(stride+1)] == 0 for y in range(h)), "filter 0"
    return w, h, depth, ctype, plte, rows

w, h, depth, ctype, plte, rows = read(f"{d}/gray.png")
assert (w, h, depth, ctype, plte) == (800, 480, 2, 0, None), (w, h, depth, ctype)
lvl = lambda r, x: (r[x // 4] >> (6 - 2 * (x % 4))) & 3
assert lvl(rows[40], 10) == 0 and lvl(rows[300], 300) == 3, "gray pixels"

# (the rect at 10,40 was recoloured purple by case 5, gray unchanged)
for name, bg, ink in (("colour-light", (255, 255, 255), (0x6A, 0x1B, 0x9A)),
                      ("colour-dark", (0, 0, 0), (0xB1, 0x97, 0xFC))):
    w, h, depth, ctype, plte, rows = read(f"{d}/{name}.png")
    assert (w, h, depth, ctype) == (320, 480, 8, 3), (name, w, h, depth, ctype)
    assert len(plte) == 64 * 3, "64 palette entries"
    rgb = lambda x, y: tuple(plte[3*rows[y][x]:3*rows[y][x]+3])
    assert rgb(300, 300) == bg, (name, rgb(300, 300))
    assert rgb(10, 40) == ink, (name, rgb(10, 40))    # recoloured rect, ink level
    assert rgb(54, 84) != bg and rgb(54, 84)[1] > rgb(54, 84)[0], (name, "green tint")
    assert rgb(0, 60)[0] > rgb(0, 60)[2], (name, "red line is red")
print("png decode: gray 2-bit + indexed light/dark verified")
EOF
echo "lcd_palette host test: ALL GREEN"
