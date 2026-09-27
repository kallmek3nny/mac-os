#!/usr/bin/env python3
"""Convert a binary PPM (P6) to PNG using only the standard library.

Why this exists
---------------
QEMU's QMP `screendump` writes a **PPM** unless you explicitly pass
`"format":"png"`. Give the filename a `.png` extension and you get a file that
looks like a PNG to every tool that trusts extensions and to no tool that reads
bytes -- the first eight bytes are `P6\n1280 ...`, not the PNG magic. That is a
silent mislabel: the dump succeeded, the size looked right (3 MB), and only an
actual decode attempt exposed it.

So this converter exists for two reasons: to rescue dumps already written that
way, and because the alternative (Pillow) is a dependency this project does not
have and the four gates deliberately do not need.

Usage:
    python tools/ppm-to-png.py IN.ppm OUT.png

Exit codes: 2 for a malformed invocation, 1 when the input is not a P6 PPM,
0 only when a PNG was written.
"""

import struct
import sys
import zlib


def read_token(data, pos):
    """Read one whitespace-delimited PPM header token, skipping # comments."""
    while pos < len(data):
        c = data[pos:pos + 1]
        if c == b"#":
            while pos < len(data) and data[pos:pos + 1] not in (b"\n", b"\r"):
                pos += 1
        elif c.isspace():
            pos += 1
        else:
            break
    start = pos
    while pos < len(data) and not data[pos:pos + 1].isspace():
        pos += 1
    return data[start:pos], pos


def ppm_to_png(src, dst):
    data = open(src, "rb").read()
    if data[:2] != b"P6":
        sys.stderr.write(
            "FAIL: %s is not a binary PPM. First 16 bytes: %s\n"
            % (src, data[:16].hex(" "))
        )
        return 1

    pos = 2
    width, pos = read_token(data, pos)
    height, pos = read_token(data, pos)
    maxval, pos = read_token(data, pos)
    # Exactly ONE whitespace byte follows maxval, and the raster starts there.
    pos += 1

    width = int(width)
    height = int(height)
    maxval = int(maxval)
    if maxval != 255:
        sys.stderr.write("FAIL: maxval %d unsupported (only 8-bit)\n" % maxval)
        return 1

    raster = data[pos:]
    expect = width * height * 3
    if len(raster) < expect:
        sys.stderr.write(
            "FAIL: raster short: %d bytes for %dx%d (need %d)\n"
            % (len(raster), width, height, expect)
        )
        return 1

    # PNG scanlines each carry a leading filter byte (0 = None).
    rows = bytearray()
    stride = width * 3
    for y in range(height):
        rows.append(0)
        rows += raster[y * stride:(y + 1) * stride]

    def chunk(tag, payload):
        out = struct.pack(">I", len(payload)) + tag + payload
        return out + struct.pack(">I", zlib.crc32(tag + payload) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(rows), 6))
    png += chunk(b"IEND", b"")

    open(dst, "wb").write(png)
    print("wrote %s: %dx%d, %d bytes" % (dst, width, height, len(png)))
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.stderr.write("usage: ppm-to-png.py IN.ppm OUT.png\n")
        sys.exit(2)
    sys.exit(ppm_to_png(sys.argv[1], sys.argv[2]))
